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
REM   build.bat            configure + build + test  (preset: default)
REM   build.bat debug      the same, for a named preset
REM
REM Presets: default, vcpkg, debug, asan, tsan, prod
REM
REM NOTE: no parenthesised if-blocks below. The VS path contains "(x86)", and
REM echoing it inside one closes the block early -- cmd then reports the
REM baffling "\Microsoft was unexpected at this time".

setlocal

set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKEDIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJADIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if not exist "%VCVARS%" goto no_vcvars

REM From P0-08b the `vcpkg` preset needs VCPKG_ROOT. Point it at the tree
REM bootstrap created, so the preset works with no extra setup.
if "%VCPKG_ROOT%"=="" if exist "C:\PycharmProjects\vcpkg\vcpkg.exe" set "VCPKG_ROOT=C:\PycharmProjects\vcpkg"
set "VCPKG_DISABLE_METRICS=1"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=default"

call "%VCVARS%" >nul 2>&1

REM vcvars does not add the bundled cmake/ninja, so prepend them ourselves.
set "PATH=%CMAKEDIR%;%NINJADIR%;%PATH%"

where cmake >nul 2>&1
if errorlevel 1 goto no_cmake
where ninja >nul 2>&1
if errorlevel 1 goto no_ninja

echo === configure [%PRESET%] ===
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

echo.
echo === build [%PRESET%] ===
cmake --build --preset %PRESET%
if errorlevel 1 exit /b 1

echo.
echo === test [%PRESET%] ===
ctest --preset %PRESET% --output-on-failure
exit /b %ERRORLEVEL%

:no_vcvars
echo ERROR: vcvars64.bat not found. Edit VSROOT at the top of this script.
echo   looked for: "%VCVARS%"
exit /b 1

:no_cmake
echo ERROR: cmake not found after running vcvars.
exit /b 1

:no_ninja
echo ERROR: ninja not found after running vcvars.
exit /b 1
