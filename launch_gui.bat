@echo off
REM Launch the Qt desktop from the clean, deployed GUI build.
REM Run this file from Explorer or a terminal; it does not require cmake/ninja.
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\gui-debug\desktop\altair_desktop.exe"
if not exist "%EXE%" (
    echo Altair GUI is not built yet.
    echo Build it with: build_gui.bat
    exit /b 1
)
start "Altair" "%EXE%"
exit /b 0