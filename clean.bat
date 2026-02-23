@echo off
REM QUICKEN Engine Clean Script (Windows)
REM
REM Usage: clean.bat          Remove build outputs + premake generated files
REM        clean.bat --all    Also remove SDL3 cmake build artifacts

setlocal

echo ========================================
echo QUICKEN Engine Clean
echo ========================================
echo.

REM Remove build outputs (obj, lib, bin)
if exist "build" (
    echo Removing build\...
    rmdir /s /q "build"
)

REM Remove Visual Studio cache
if exist ".vs" (
    echo Removing .vs\...
    rmdir /s /q ".vs"
)

REM Remove premake-generated files
if exist "QUICKEN.sln" (
    echo Removing premake-generated solution and projects...
    del /q "QUICKEN.sln" 2>nul
)
del /q *.vcxproj 2>nul
del /q *.vcxproj.filters 2>nul
del /q *.vcxproj.user 2>nul
if exist "Makefile" del /q "Makefile" 2>nul
del /q *.make 2>nul

REM Full clean: also remove SDL3 build artifacts
if "%1"=="--all" (
    echo.
    echo Removing SDL3 build artifacts...
    if exist "external\SDL3\build" rmdir /s /q "external\SDL3\build"
    if exist "external\SDL3\build-linux" rmdir /s /q "external\SDL3\build-linux"
)

echo.
echo Clean complete.
if not "%1"=="--all" (
    echo Use "clean.bat --all" to also remove SDL3 build artifacts.
)

endlocal
