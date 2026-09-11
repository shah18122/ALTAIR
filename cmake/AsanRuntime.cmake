# cmake/AsanRuntime.cmake -- an ASAN binary must be able to find its runtime.
#
# MSVC's /fsanitize=address links every instrumented executable against
# clang_rt.asan_dynamic-x86_64.dll, which ships next to cl.exe and is NOT
# copied anywhere by the compiler. So an asan build is only runnable from a
# shell whose PATH happens to contain the compiler's directory -- which is
# exactly what vcvars sets up and exactly what every other way of starting a
# program does not:
#
#   - double-clicking the exe in Explorer
#   - a fresh PowerShell or cmd window
#   - ctest run without build.bat
#   - any test whose PATH is overridden (desktop_fits, desktop_bar_csv and
#     desktop_filter did exactly that -- see desktop/CMakeLists.txt)
#
# Each of those fails with "clang_rt.asan_dynamic-x86_64.dll was not found",
# and on Windows that is a HARD-ERROR DIALOG owned by csrss.exe rather than by
# the program, so a test run under ctest does not crash, it sits blocked on an
# invisible box until the timeout. Four asan "failures" were this and nothing
# else.
#
# The fix is the one the Qt runtime already gets from windeployqt: put the DLL
# NEXT TO THE EXECUTABLE, where the loader always looks first. Done as a
# post-build step on every executable target, so it survives a clean rebuild
# and needs nobody to remember it.
#
# Both variants are copied. The plain one is what /MD code imports; the _dbg
# one is what /MDd imports, and a Debug-CRT target that picked it up would fail
# the same way. Two small files is cheaper than finding out which.

function(_altair_collect_executables dir out)
    set(_found "")
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            list(APPEND _found ${_t})
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sd IN LISTS _subdirs)
        _altair_collect_executables("${_sd}" _sub)
        list(APPEND _found ${_sub})
    endforeach()
    set(${out} ${_found} PARENT_SCOPE)
endfunction()

function(altair_stage_asan_runtime)
    if(NOT ALTAIR_ENABLE_ASAN OR NOT MSVC)
        return()
    endif()

    get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_dlls "")
    foreach(_name clang_rt.asan_dynamic-x86_64.dll
                  clang_rt.asan_dbg_dynamic-x86_64.dll)
        if(EXISTS "${_cl_dir}/${_name}")
            list(APPEND _dlls "${_cl_dir}/${_name}")
        endif()
    endforeach()

    if(NOT _dlls)
        # REFUSE rather than configure a build that cannot run. The most
        # likely cause is a toolset that ships the runtime somewhere new, and a
        # silent skip here would recreate exactly the invisible hang this file
        # exists to prevent.
        message(FATAL_ERROR
            "ASAN is on but clang_rt.asan_dynamic-x86_64.dll is not next to the "
            "compiler (${_cl_dir}). Every instrumented executable would fail "
            "to start. Find the DLL and update cmake/AsanRuntime.cmake.")
    endif()

    _altair_collect_executables("${CMAKE_SOURCE_DIR}" _exes)
    list(LENGTH _exes _n)

    # ONE STAGING TARGET, AND EVERY EXECUTABLE DEPENDS ON IT.
    #
    # The obvious version -- add_custom_command(TARGET x POST_BUILD) for each
    # executable -- is refused by CMake unless it is called from the directory
    # that CREATED x, and this runs once from the top. The first draft did it
    # anyway and configure failed 128 times, once per target. add_dependencies
    # has no such restriction, so the copies live in one target that runs
    # before anything links, and a deleted DLL comes back on the next build
    # rather than on the next reconfigure.
    set(_dirs "")
    foreach(_t IN LISTS _exes)
        get_target_property(_bin ${_t} BINARY_DIR)
        get_target_property(_out ${_t} RUNTIME_OUTPUT_DIRECTORY)
        if(_out)
            list(APPEND _dirs "${_out}")
        else()
            list(APPEND _dirs "${_bin}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _dirs)

    set(_cmds "")
    foreach(_d IN LISTS _dirs)
        list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E make_directory "${_d}")
        list(APPEND _cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different
                          ${_dlls} "${_d}")
    endforeach()
    add_custom_target(altair_asan_runtime ALL ${_cmds}
        COMMENT "Staging the ASAN runtime next to every executable"
        VERBATIM)
    foreach(_t IN LISTS _exes)
        add_dependencies(${_t} altair_asan_runtime)
    endforeach()

    list(LENGTH _dirs _nd)
    message(STATUS
            "altair: ASAN runtime staged into ${_nd} folder(s) for ${_n} "
            "executable(s), from ${_cl_dir}")
endfunction()
