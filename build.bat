@echo off
REM QUICKEN Engine Build Script (Windows)
REM Automatically finds MSBuild and builds the project.
REM
REM Usage: build.bat [Release|Debug|RelWithDebInfo]
REM
REM premake5 is optional if the .sln already exists.
REM To regenerate from WSL: wsl bash -c "cd /mnt/h/quicken/quicken-engine && premake5 vs2022"

setlocal enabledelayedexpansion

REM Default to Release build
set CONFIG=Release
if not "%1"=="" set CONFIG=%1

echo ========================================
echo QUICKEN Engine Build Script
echo ========================================
echo Configuration: %CONFIG%
echo.

REM Check for cmake
where cmake.exe >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] cmake.exe not found in PATH!
    echo Install CMake and add it to your PATH.
    exit /b 1
)

REM Ensure SDL3 submodule is initialized
if not exist "external\SDL3\CMakeLists.txt" (
    echo SDL3 submodule not found. Initializing...
    git submodule update --init
)

REM Build SDL3 if not already built
if not exist "external\SDL3\build\Release\SDL3.dll" (
    echo Building SDL3...
    cmake -S external\SDL3 -B external\SDL3\build -G "Visual Studio 17 2022" -A x64
    cmake --build external\SDL3\build --config Release
)

REM Try to regenerate VS solution with premake5
where premake5.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo Generating VS2022 solution...
    premake5.exe vs2022
) else (
    if exist "QUICKEN.sln" (
        echo [NOTE] premake5 not in PATH. Using existing QUICKEN.sln.
        echo        To regenerate: wsl bash -c "cd /mnt/h/quicken/quicken-engine && premake5 vs2022"
    ) else (
        echo [ERROR] premake5.exe not found and no QUICKEN.sln exists!
        echo.
        echo Option 1: Install premake5 and add to PATH
        echo Option 2: Generate from WSL: wsl bash -c "cd /mnt/h/quicken/quicken-engine && premake5 vs2022"
        exit /b 1
    )
)

REM Try to find MSBuild in common locations (prioritize 2022 over 2019)
set "MSBUILD="

REM VS 2022 Community
if "%MSBUILD%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
)

REM VS 2022 Professional
if "%MSBUILD%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
)

REM VS 2022 Enterprise
if "%MSBUILD%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
)

REM VS 2022 Build Tools
if "%MSBUILD%"=="" if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)

REM VS 2019 fallback (only if 2022 not found)
if "%MSBUILD%"=="" if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"
)

REM Check if we found MSBuild
if "%MSBUILD%"=="" (
    echo [ERROR] MSBuild not found!
    echo.
    echo Please install Visual Studio 2022 or Visual Studio Build Tools.
    echo Or run this script from a Developer Command Prompt.
    echo.
    pause
    exit /b 1
)

echo Found MSBuild: %MSBUILD%
echo.

REM Build the solution
echo Building...
"%MSBUILD%" QUICKEN.sln /p:Configuration=%CONFIG% /p:Platform=x64 /m /v:minimal /nologo

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================
    echo Build succeeded!
    echo ========================================
    echo.
    echo Client:    build\bin\%CONFIG%-windows-x86_64\quicken.exe
    echo Server:    build\bin\%CONFIG%-windows-x86_64\quicken-server.exe
    echo.
) else (
    echo.
    echo ========================================
    echo Build failed!
    echo ========================================
    echo.
    exit /b %ERRORLEVEL%
)

endlocal
