@echo off
REM PuffyEngine Build Script
REM 
REM This script builds PuffyEngine using Visual Studio.
REM Make sure you have:
REM   1. Visual Studio 2019 or 2022 with C++ workload
REM   2. CMake 3.16+ in your PATH
REM   3. Steamworks SDK extracted to ./steamworks_sdk (or set STEAMWORKS_SDK_PATH)

setlocal enabledelayedexpansion

echo ================================================
echo  PuffyEngine Build Script
echo ================================================
echo.

REM Check for CMake
where cmake >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: CMake not found in PATH
    echo Please install CMake from https://cmake.org/download/
    exit /b 1
)

REM Set Steamworks SDK path if not already set
if not defined STEAMWORKS_SDK_PATH (
    set STEAMWORKS_SDK_PATH=%~dp0steamworks_sdk
)

REM Check if Steamworks SDK exists
if not exist "%STEAMWORKS_SDK_PATH%\public\steam\steam_api.h" (
    echo ERROR: Steamworks SDK not found at: %STEAMWORKS_SDK_PATH%
    echo.
    echo Please either:
    echo   1. Extract the Steamworks SDK to: %~dp0steamworks_sdk
    echo   2. Set STEAMWORKS_SDK_PATH environment variable
    echo.
    echo Download from: https://partner.steamgames.com/downloads/steamworks_sdk.zip
    exit /b 1
)

echo Found Steamworks SDK at: %STEAMWORKS_SDK_PATH%
echo.

REM Create build directory
if not exist build mkdir build
cd build

REM Detect Visual Studio version and run CMake
echo Running CMake...
cmake .. -G "Visual Studio 17 2022" -A x64 -DSTEAMWORKS_SDK_PATH="%STEAMWORKS_SDK_PATH%"
if %errorlevel% neq 0 (
    REM Try VS 2019 if VS 2022 failed
    cmake .. -G "Visual Studio 16 2019" -A x64 -DSTEAMWORKS_SDK_PATH="%STEAMWORKS_SDK_PATH%"
    if %errorlevel% neq 0 (
        echo ERROR: CMake configuration failed
        exit /b 1
    )
)

echo.
echo Building Release configuration...
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo ================================================
echo  Build successful!
echo ================================================
echo.
echo Output: %cd%\Release\PuffyEngine.exe
echo.
echo To run:
echo   1. Make sure Steam is running
echo   2. Run: Release\PuffyEngine.exe
echo.
echo Command line options:
echo   --puffyport ^<port^>     HTTP server port (default: 19996)
echo   --puffydir ^<path^>      Game content directory (default: ./game/)
echo   --puffywidth ^<pixels^>  Window width (default: 80%% of screen)
echo   --puffyheight ^<pixels^> Window height (default: 80%% of screen)
echo.

cd ..
exit /b 0