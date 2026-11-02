@echo off
REM Altair build wrapper.
REM
REM CLAUDE.md documents `cmake --preset default`, and that is still the real
REM command. This wrapper exists because on this box neither cmake nor ninja is
REM on PATH, and vcvars64.bat cannot export into PowerShell -- it runs in a
REM child process and its environment dies with it. Both ship inside Visual
REM Studio Build Tools, so this calls vcvars in the SAME cmd process, then runs
REM cmake.
REM
REM   build.bat net             build what changed, no tests (fast, after every edit)
REM   build.bat net app         only the apps: desktop, price service, live engine
REM                             (skips the ~240 test programs: fastest)
REM   build.bat net test        build everything, then run the tests in parallel
REM   build.bat net full        configure afresh + build + test (the old behaviour)
REM   build.bat                 the same, preset "default"
REM
REM Configure runs only when the build folder has no cache yet (or for "full"):
REM after that, ninja re-runs CMake by itself whenever a CMakeLists.txt changes.
REM
REM Presets: default, vcpkg, debug, asan, tsan, prod
REM
REM NOTE: no parenthesised if-blocks below. The VS path contains "(x86)", and
REM echoing it inside one closes the block early -- cmd then reports the
REM baffling "\Microsoft was unexpected at this time".

setlocal

REM Run from the folder this script is in, wherever the project now lives
REM (it moved from C: to D:\altair once, and nothing below may assume a path).
set "SRCDIR=%~dp0"
if "%SRCDIR:~-1%"=="\" set "SRCDIR=%SRCDIR:~0,-1%"
pushd "%SRCDIR%"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=default"
set "MODE=%~2"
if "%MODE%"=="" set "MODE=build"
if /I not "%MODE%"=="build" if /I not "%MODE%"=="app" if /I not "%MODE%"=="test" if /I not "%MODE%"=="full" goto bad_mode

REM Visual Studio Build Tools: ask vswhere first, then the usual folder.
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKEDIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJADIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if not exist "%VCVARS%" goto no_vcvars

REM vcpkg, for the `vcpkg` and `net` presets: VCPKG_ROOT when it points at a
REM real tree, else the first of the usual places that has vcpkg.exe.
if defined VCPKG_ROOT if not exist "%VCPKG_ROOT%\vcpkg.exe" set "VCPKG_ROOT="
for %%d in ("%SRCDIR%\vcpkg" "%SRCDIR%\..\vcpkg" "D:\vcpkg" "C:\vcpkg" "C:\PycharmProjects\vcpkg" "C:\src\vcpkg" "%USERPROFILE%\vcpkg") do if not defined VCPKG_ROOT if exist "%%~d\vcpkg.exe" set "VCPKG_ROOT=%%~fd"
set "VCPKG_DISABLE_METRICS=1"
set "NEEDS_VCPKG="
if /I "%PRESET%"=="vcpkg" set "NEEDS_VCPKG=1"
if /I "%PRESET%"=="net" set "NEEDS_VCPKG=1"
if defined NEEDS_VCPKG if not defined VCPKG_ROOT goto no_vcpkg
if defined NEEDS_VCPKG echo vcpkg: %VCPKG_ROOT%

REM A build tree configured from another folder is refused by CMake ("does not
REM match the source ... used to generate cache"): a copied or moved project
REM carries one. Its cache is dropped so the preset configures afresh; the
REM compiled objects stay and are reused where they still match.
set "SRCFWD=%SRCDIR:\=/%"
set "CACHE=%SRCDIR%\build\%PRESET%\CMakeCache.txt"
if not exist "%CACHE%" goto cache_ok
findstr /X /I /L /C:"CMAKE_HOME_DIRECTORY:INTERNAL=%SRCFWD%" "%CACHE%" >nul
if not errorlevel 1 goto cache_ok
echo NOTE: build\%PRESET% was configured from another folder; configuring it afresh for %SRCDIR%.
del /q "%CACHE%"
if exist "%SRCDIR%\build\%PRESET%\CMakeFiles" rmdir /s /q "%SRCDIR%\build\%PRESET%\CMakeFiles"
:cache_ok

call "%VCVARS%" >nul 2>&1

REM vcvars does not add the bundled cmake/ninja, so prepend them ourselves.
set "PATH=%CMAKEDIR%;%NINJADIR%;%PATH%"

where cmake >nul 2>&1
if errorlevel 1 goto no_cmake
where ninja >nul 2>&1
if errorlevel 1 goto no_ninja

REM `if errorlevel 1` tests errorlevel >= 1, so it is FALSE for a NEGATIVE
REM exit code -- and a failed MSVC link returns 4294967295, i.e. -1. That trap
REM let a link failure through and ran ctest against a STALE binary, which
REM then "passed" tests for code that had never been compiled. NEQ 0 reads the
REM actual value and catches both signs.
REM A program that is still running cannot be overwritten: the link then fails
REM with LNK1168 after a long compile. Say so up front instead. Nothing is
REM closed for you -- the live engine may be holding paper positions.
set "RUNNING="
tasklist /FI "IMAGENAME eq altair_desktop.exe" 2>nul | find /I "altair_desktop.exe" >nul && set "RUNNING=%RUNNING% altair_desktop.exe"
tasklist /FI "IMAGENAME eq altair_price_service.exe" 2>nul | find /I "altair_price_service.exe" >nul && set "RUNNING=%RUNNING% altair_price_service.exe"
tasklist /FI "IMAGENAME eq altair_live_engine.exe" 2>nul | find /I "altair_live_engine.exe" >nul && set "RUNNING=%RUNNING% altair_live_engine.exe"
if defined RUNNING goto still_running

if /I "%MODE%"=="full" goto configure
if not exist "%CACHE%" goto configure
if exist "%SRCDIR%\build\%PRESET%\build.ninja" goto build
:configure
echo === configure [%PRESET%] ===
cmake --preset %PRESET%
if %ERRORLEVEL% NEQ 0 goto configure_failed
echo.

:build
if /I "%MODE%"=="app" goto build_apps
echo === build [%PRESET%] ===
cmake --build --preset %PRESET%
if %ERRORLEVEL% NEQ 0 exit /b 1
goto built

:build_apps
REM Only the programs you run. A target the configure did not create (no Qt,
REM so no desktop; no vcpkg net, so no price service) is left out, not an error.
set "APPS="
findstr /L /C:"build altair_desktop:" "%SRCDIR%\build\%PRESET%\build.ninja" >nul 2>&1
if not errorlevel 1 set "APPS=%APPS% altair_desktop"
findstr /L /C:"build altair_price_service:" "%SRCDIR%\build\%PRESET%\build.ninja" >nul 2>&1
if not errorlevel 1 set "APPS=%APPS% altair_price_service"
findstr /L /C:"build altair_live_engine:" "%SRCDIR%\build\%PRESET%\build.ninja" >nul 2>&1
if not errorlevel 1 set "APPS=%APPS% altair_live_engine"
if not defined APPS goto no_apps
echo === build apps [%PRESET%]:%APPS% ===
cmake --build --preset %PRESET% --target%APPS%
if %ERRORLEVEL% NEQ 0 exit /b 1

:built
echo.
if /I "%MODE%"=="build" goto skip_tests
if /I "%MODE%"=="app" goto skip_tests
REM A preset that deliberately builds NO test binaries -- `prod` sets
REM ALTAIR_BUILD_TESTS=OFF so a shipped tree carries no test executables -- has
REM nothing for ctest to run, and ctest calls that an error. Reporting a clean
REM Release build as a failure trains you to ignore the exit code, so the
REM no-tests case is named and passed, and every other ctest failure still
REM fails. It says WHERE the tests were run instead, because "no tests" must
REM never read as "tests passed".
echo === test [%PRESET%] ===
ctest --preset %PRESET% --output-on-failure -j %NUMBER_OF_PROCESSORS%
set CTEST_RC=%ERRORLEVEL%
if %CTEST_RC% NEQ 0 (
    if /I "%PRESET%"=="prod" (
        echo.
        echo NOTE: `prod` builds no test binaries by design ^(ALTAIR_BUILD_TESTS=OFF^).
        echo       This is NOT a passing test run. Verify with: build.bat default
        exit /b 0
    )
)
exit /b %CTEST_RC%

:skip_tests
echo Built. Tests not run: "build.bat %PRESET% test" runs them.
exit /b 0

:still_running
echo ERROR: still running:%RUNNING%
echo   Windows will not let the build overwrite a running program (LNK1168).
echo   Close them first, e.g.:  taskkill /IM altair_live_engine.exe /IM altair_price_service.exe /IM altair_desktop.exe
exit /b 1

:bad_mode
echo ERROR: unknown mode "%MODE%". Use: build, app, test or full.
echo   e.g.  build.bat net        build.bat net app        build.bat net test
exit /b 1

:no_apps
echo ERROR: none of altair_desktop, altair_price_service, altair_live_engine is in build\%PRESET%.
echo   Run "build.bat %PRESET% full" once to configure it.
exit /b 1

:no_vcvars
echo ERROR: Visual Studio Build Tools (C++ workload) not found.
echo   looked for: "%VCVARS%"
echo   Install "Desktop development with C++" from the Visual Studio Installer.
exit /b 1

:no_vcpkg
echo ERROR: the "%PRESET%" preset needs vcpkg, and none was found.
echo   Looked in: %%VCPKG_ROOT%%, .\vcpkg, ..\vcpkg, D:\vcpkg, C:\vcpkg, C:\PycharmProjects\vcpkg, C:\src\vcpkg, %%USERPROFILE%%\vcpkg
echo   Install it once, on an NTFS drive (C: is fine):
echo     git clone https://github.com/microsoft/vcpkg C:\vcpkg
echo     C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
echo   or point at a copy you have:  set VCPKG_ROOT=X:\path\to\vcpkg
exit /b 1

:configure_failed
echo.
echo CMake configure failed for preset "%PRESET%" in %SRCDIR%.
echo   - The first "CMake Error" lines above say why.
echo   - To start a build tree completely afresh: rmdir /s /q build\%PRESET%
if not defined NEEDS_VCPKG exit /b 1
echo   - vcpkg (%VCPKG_ROOT%) on a drive that records no file owners (exFAT/FAT32) fails
echo     inside git with "dubious ownership": git config --global --add safe.directory "%VCPKG_ROOT:\=/%"
exit /b 1

:no_cmake
echo ERROR: cmake not found after running vcvars.
exit /b 1

:no_ninja
echo ERROR: ninja not found after running vcvars.
exit /b 1
