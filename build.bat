@echo off
REM QUICKEN Engine Build Script
REM Automatically finds MSBuild and builds the project

setlocal enabledelayedexpansion

REM Default to Release build
set CONFIG=Release
if not "%1"=="" set CONFIG=%1

echo ========================================
echo QUICKEN Engine Build Script
echo ========================================
echo Configuration: %CONFIG%
echo.

REM Regenerate VS solution to ensure it targets the current platform
where premake5.exe >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] premake5.exe not found in PATH!
    echo Install premake5 and add it to your PATH.
    exit /b 1
)
echo Generating VS2022 solution...
premake5.exe vs2022

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
    echo Executable: build\bin\%CONFIG%-windows-x86_64\quicken.exe
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
