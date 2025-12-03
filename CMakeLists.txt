# PuffyEngine - Steam HTMLSurface Web Game Engine
# 
# Build Instructions:
#   1. Set STEAMWORKS_SDK_PATH to your Steamworks SDK location
#   2. mkdir build && cd build
#   3. cmake .. -G "Visual Studio 17 2022" -A x64
#   4. cmake --build . --config Release
#
# Alternatively, use the provided build.bat script.

cmake_minimum_required(VERSION 3.16)
project(PuffyEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Steamworks SDK path - set this to your SDK location
if(NOT DEFINED STEAMWORKS_SDK_PATH)
    set(STEAMWORKS_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/steamworks_sdk" CACHE PATH "Path to Steamworks SDK")
endif()

# Verify Steamworks SDK exists
if(NOT EXISTS "${STEAMWORKS_SDK_PATH}/public/steam/steam_api.h")
    message(FATAL_ERROR 
        "Steamworks SDK not found at: ${STEAMWORKS_SDK_PATH}\n"
        "Please set STEAMWORKS_SDK_PATH to your Steamworks SDK location.\n"
        "Download from: https://partner.steamgames.com/downloads/steamworks_sdk.zip"
    )
endif()

# Main executable
add_executable(PuffyEngine
    puffy.cpp
)

# Include directories
target_include_directories(PuffyEngine PRIVATE
    "${STEAMWORKS_SDK_PATH}/public"
)

# Library directories
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(STEAM_LIB_DIR "${STEAMWORKS_SDK_PATH}/redistributable_bin/win64")
    set(STEAM_LIB_NAME "steam_api64")
else()
    set(STEAM_LIB_DIR "${STEAMWORKS_SDK_PATH}/redistributable_bin")
    set(STEAM_LIB_NAME "steam_api")
endif()

target_link_directories(PuffyEngine PRIVATE
    "${STEAM_LIB_DIR}"
)

# Link libraries
target_link_libraries(PuffyEngine PRIVATE
    ${STEAM_LIB_NAME}
    ws2_32
    xinput
    shcore
)

# Windows-specific settings
if(MSVC)
    # Suppress deprecated function warnings
    target_compile_definitions(PuffyEngine PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    
    # Use static runtime for smaller deployment
    set_property(TARGET PuffyEngine PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    
    # Disable specific warnings
    target_compile_options(PuffyEngine PRIVATE
        /W4             # High warning level
        /wd4100         # Unreferenced formal parameter
        /wd4189         # Local variable initialized but not referenced
    )
    
    # Enable optimizations for size in release
    target_compile_options(PuffyEngine PRIVATE
        $<$<CONFIG:Release>:/O1 /GL /Gy /Gw>
    )
    target_link_options(PuffyEngine PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
    )
endif()

# Copy Steam DLL to output directory
add_custom_command(TARGET PuffyEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${STEAM_LIB_DIR}/${STEAM_LIB_NAME}.dll"
        "$<TARGET_FILE_DIR:PuffyEngine>"
    COMMENT "Copying Steam API DLL..."
)

# Copy game directory to output
add_custom_command(TARGET PuffyEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/game"
        "$<TARGET_FILE_DIR:PuffyEngine>/game"
    COMMENT "Copying game content..."
)

# Create steam_appid.txt for development (replace 480 with your actual App ID)
add_custom_command(TARGET PuffyEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "480" > "$<TARGET_FILE_DIR:PuffyEngine>/steam_appid.txt"
    COMMENT "Creating steam_appid.txt..."
)

# Installation
install(TARGETS PuffyEngine RUNTIME DESTINATION .)
install(DIRECTORY game/ DESTINATION game)
install(FILES "${STEAM_LIB_DIR}/${STEAM_LIB_NAME}.dll" DESTINATION .)

message(STATUS "PuffyEngine configured successfully")
message(STATUS "  Steamworks SDK: ${STEAMWORKS_SDK_PATH}")
message(STATUS "  Steam library: ${STEAM_LIB_DIR}/${STEAM_LIB_NAME}.lib")
