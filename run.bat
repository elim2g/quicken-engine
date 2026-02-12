@echo off
REM QUICKEN Engine Run Script
REM Builds and runs the game

setlocal

set CONFIG=Release
if not "%1"=="" set CONFIG=%1

echo Building QUICKEN Engine (%CONFIG%)...
call build.bat %CONFIG%

if %ERRORLEVEL% NEQ 0 (
    echo Build failed, not running.
    exit /b 1
)

echo.
echo ========================================
echo Running QUICKEN Engine
echo ========================================
echo.

set EXE=build\bin\%CONFIG%-windows-x86_64\quicken.exe

if not exist "%EXE%" (
    echo [ERROR] Executable not found: %EXE%
    exit /b 1
)

"%EXE%"

endlocal
