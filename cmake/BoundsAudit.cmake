# cmake/BoundsAudit.cmake -- hard rule 11, enforced at configure time.
#
# P33-06. Rule 11 says a fixed bound must refuse what exceeds it, prove it
# cannot be reached, or truncate visibly. It was written after the same bug was
# found five times and was enforced by review -- which is what had failed five
# times.
#
# WHAT THIS CAN AND CANNOT DECIDE, STATED FIRST.
#
# "This bound can be exceeded and does not refuse" is not a textual fact. It
# needs to know what the caller can pass, and no regex knows that. So this does
# NOT try to decide the rule. It finds the SHAPE the rule is about -- a clamp
# -- and requires that a human wrote down which arm of the rule that clamp
# takes.
#
#     A < B ? A : B          clamp to B
#     A > B ? B : A          clamp to B
#     std::min(A, kSomething)
#
# Six of the seven real findings had exactly this shape. The seventh -- the
# chi-square table that ended one short of kMaxStates -- did not, and this
# check would not have caught it. Said here so nobody reads a green configure
# as "rule 11 holds".
#
# THE ANNOTATION IS THE POINT, NOT THE DETECTION.
#
# A clamp is legal. What is not legal is a clamp nobody thought about. A site
# passes when `RULE 11` appears on it or in the twelve lines above it, and the
# annotation says which arm it takes:
#
#     // RULE 11: proven unreachable -- lambda > 12 is refused above.
#     // RULE 11: counted -- KiteLoadReport::underlying_truncated.
#     // RULE 11: safe-side clamp -- an unknown action gets the SMALLEST size.
#
# Deliberately a comment and not a macro. The value is the sentence a reviewer
# had to write, and a macro would let somebody satisfy the check without
# producing one.
#
# PRECISION OVER RECALL, ON PURPOSE.
#
# The ternary test requires the clamped expression on BOTH sides --
# `x < K ? x : K` -- because `mp < 10 ? mp + 3 : mp - 9` is month arithmetic
# and not a clamp. A check that cries wolf is worse than no check: the
# annotations stop meaning anything and the next real one gets waved through.
#
# ── TWO WAYS THIS CHECK LIED BEFORE IT WORKED ────────────────────────────────
#
# Both found by planting a violation and watching it pass, which is the only
# way to test a check.
#
# 1. The ternary's '?' was backslash-escaped. CMake collapses that to a bare
#    '?' before the regex engine sees it, giving " *? *" -- "nested *?+",
#    which does not COMPILE. An if(MATCHES) whose regex will not compile
#    evaluates FALSE. A one-character class needs no escape, so it is [?].
#
# 2. The file was split into lines with string(REPLACE "\n" ";") and read back
#    with list(GET). CMake list semantics mangled it: analytics/hurst.hpp is
#    319 lines and came back as 172 elements with the whole tail in the last
#    one, so nothing past the middle of any file was ever examined. It now
#    walks the text with string(FIND) and string(SUBSTRING), which has no list
#    semantics to get wrong.
#
# Both failures reported "clean". That is the exact shape of the bug this rule
# exists to catch, committed twice inside the checker for it.

function(altair_audit_bounds)
    set(_dirs core instruments broker feed book analytics risk oms features
              strategies backtest models flagging server live desktop app)
    set(_bad "")
    set(_seen 0)
    # Counted explicitly, NOT with list(LENGTH _bad). The offending source line
    # ends in a semicolon, so list(APPEND) splits one finding into two elements
    # and the message said "2 clamp sites" for one. A checker that miscounts
    # its own findings is one nobody believes the third time.
    set(_nbad 0)

    foreach(_dir IN LISTS _dirs)
        file(GLOB_RECURSE _srcs
             "${CMAKE_SOURCE_DIR}/${_dir}/*.hpp"
             "${CMAKE_SOURCE_DIR}/${_dir}/*.cpp")
        foreach(_f IN LISTS _srcs)
            if(_f MATCHES "/tests/" OR _f MATCHES "/tools/")
                continue()
            endif()

            file(READ "${_f}" _rest)
            set(_lineno 0)
            # How many more lines an annotation still covers. Set when RULE 11
            # is seen and decremented every line -- a rolling window without
            # having to hold the lines.
            set(_ann 0)

            while(TRUE)
                string(LENGTH "${_rest}" _rl)
                if(_rl EQUAL 0)
                    break()
                endif()
                string(FIND "${_rest}" "\n" _nl)
                if(_nl LESS 0)
                    set(_line "${_rest}")
                    set(_rest "")
                else()
                    string(SUBSTRING "${_rest}" 0 ${_nl} _line)
                    math(EXPR _after "${_nl} + 1")
                    math(EXPR _remain "${_rl} - ${_after}")
                    string(SUBSTRING "${_rest}" ${_after} ${_remain} _rest)
                endif()
                math(EXPR _lineno "${_lineno} + 1")

                if(_line MATCHES "RULE 11")
                    set(_ann 13)
                elseif(_ann GREATER 0)
                    math(EXPR _ann "${_ann} - 1")
                endif()

                string(STRIP "${_line}" _st)
                if(_st MATCHES "^//")
                    continue()
                endif()

                set(_hit "")

                # A < B ? A : B -- the clamped expression on both sides.
                if(_line MATCHES
                   "([A-Za-z_0-9.()>:-]+) *< *([A-Za-z_0-9]+) *[?] *([A-Za-z_0-9.()>:-]+) *: *([A-Za-z_0-9]+)")
                    if("${CMAKE_MATCH_1}" STREQUAL "${CMAKE_MATCH_3}" AND
                       "${CMAKE_MATCH_2}" STREQUAL "${CMAKE_MATCH_4}")
                        set(_hit "clamp to ${CMAKE_MATCH_2}")
                    endif()
                endif()

                # A > B ? B : A
                if(NOT _hit AND _line MATCHES
                   "([A-Za-z_0-9.()>:-]+) *> *([A-Za-z_0-9]+) *[?] *([A-Za-z_0-9]+) *: *([A-Za-z_0-9.()>:-]+)")
                    if("${CMAKE_MATCH_1}" STREQUAL "${CMAKE_MATCH_4}" AND
                       "${CMAKE_MATCH_2}" STREQUAL "${CMAKE_MATCH_3}")
                        set(_hit "clamp to ${CMAKE_MATCH_2}")
                    endif()
                endif()

                # std::min against a NAMED capacity. A min of two runtime
                # values is ordinary arithmetic; one against kMaxSomething is
                # a capacity decision.
                if(NOT _hit AND
                   _line MATCHES "std::min *\\([^)]*(k[A-Z][A-Za-z0-9_]*)")
                    set(_hit "std::min against ${CMAKE_MATCH_1}")
                endif()

                if(NOT _hit)
                    continue()
                endif()
                math(EXPR _seen "${_seen} + 1")

                if(_ann EQUAL 0)
                    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_f}")
                    math(EXPR _nbad "${_nbad} + 1")
                    string(APPEND _bad
                           "  ${_rel}:${_lineno}\n      ${_hit}\n      ${_st}\n")
                endif()
            endwhile()
        endforeach()
    endforeach()

    if(_nbad GREATER 0)
        message(FATAL_ERROR
            "RULE 11: ${_nbad} clamp site(s) with no stated justification.\n\n"
            "${_bad}\n"
            "A fixed bound must REFUSE what exceeds it, PROVE it cannot be "
            "reached, or TRUNCATE VISIBLY -- counted, carried in the result, "
            "and on screen. Silently clamping is banned, and where a clamp is "
            "unavoidable it clamps toward the SAFE side.\n\n"
            "This has been found seven times in this tree. Every one returned "
            "a plausible number, and every one reported a metric computed "
            "over the same truncated slice -- so the number that would have "
            "exposed it agreed with it.\n\n"
            "If the clamp is right, say why in a comment containing RULE 11, "
            "on the line or within twelve lines above it. The sentence is the "
            "point: this check cannot decide whether a bound is safe, only "
            "whether somebody has said so.")
    endif()
    message(STATUS
            "altair: bounds audit clean -- ${_seen} clamp site(s), all stated")
endfunction()
