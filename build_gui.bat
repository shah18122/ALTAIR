@echo off
REM Configure/build a clean GUI tree. The default build directory may be held
REM by an older GUI process, so this intentionally uses build\gui-debug.
setlocal
REM Visual Studio Build Tools: vswhere first, then the usual folder. No
REM parenthesised if-blocks: the path holds "(x86)" (see build.bat).
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKEDIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJADIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if not exist "%VCVARS%" goto no_vcvars
call "%VCVARS%" >nul 2>&1
set "PATH=%CMAKEDIR%;%NINJADIR%;%PATH%"
pushd "%~dp0"
cmake -S . -B build\gui-debug -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DALTAIR_BUILD_TESTS=OFF -DALTAIR_BUILD_BENCH=OFF
if errorlevel 1 exit /b 1
cmake --build build\gui-debug --target altair_desktop --parallel 4
popd
exit /b %ERRORLEVEL%

:no_vcvars
echo Visual Studio Build Tools were not found: "%VCVARS%"
exit /b 1
