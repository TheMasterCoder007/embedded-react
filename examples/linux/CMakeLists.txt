cmake_minimum_required(VERSION 3.16)
project(embedded-react-desktop C)

# ---------------------------------------------------------------------------
# embedded-react-desktop — Linux/Windows SDL2 desktop demo
#
# Standalone example project. Pulls in the engine and SDL2 backend from the
# monorepo via relative paths so the example can be configured and built
# directly from this directory without touching the top-level CMakeLists.
#
# Prerequisites:
#   - CMake 3.16+
#   - SDL2 2.0.6+ (SDL_ComposeCustomBlendMode required)
#
# Configure:
#   cmake -S . -B build
#   cmake --build build
#
# On Windows with vcpkg:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
#   cmake --build build
# ---------------------------------------------------------------------------

find_package(SDL2 REQUIRED)

# Desktop demo always enables shadows and uses a larger scratch buffer so that
# panel-sized nodes can receive box shadows without graceful-degradation silent skips.
set(ERUI_SHADOWS ON CACHE BOOL "Enable box shadow rasterizer" FORCE)
set(ERUI_SCRATCH_W "512" CACHE STRING "Scratch buffer width in pixels" FORCE)
set(ERUI_SCRATCH_H "512" CACHE STRING "Scratch buffer height in pixels" FORCE)

add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../engine
        ${CMAKE_CURRENT_BINARY_DIR}/engine
)

add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/sdl
        ${CMAKE_CURRENT_BINARY_DIR}/backends/sdl
)

add_executable(embedded-react-desktop main.c)

target_link_libraries(embedded-react-desktop
        PRIVATE
        er-backend-sdl
)

# On Windows, copy SDL2.dll next to the executable after each build so the
# binary can be run directly from the build output directory (CLion, Explorer, etc.)
if(WIN32)
    add_custom_command(TARGET embedded-react-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:embedded-react-desktop>
    )
endif()
