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

# ---------------------------------------------------------------------------
# Desktop demo — enable every engine feature and use a large scratch buffer
# so that panel-sized nodes work with shadows, opacity, and 3D transforms.
# ---------------------------------------------------------------------------
set(ERUI_SHADOWS           ON             CACHE BOOL   "Enable box shadow rasterizer"               FORCE)
set(ERUI_BORDER_AA         ON             CACHE BOOL   "Anti-aliased corner edges"                  FORCE)
set(ERUI_3D_TRANSFORMS     ON             CACHE BOOL   "Enable rotateX/rotateY/perspective"         FORCE)
set(ERUI_BILINEAR_SCALE    ON             CACHE BOOL   "Bilinear image scaling"                     FORCE)
set(ERUI_GRADIENT          ON             CACHE BOOL   "Enable gradient rasterizer"                 FORCE)
set(ERUI_GRADIENT_RADIAL   ON             CACHE BOOL   "Enable radial gradients"                    FORCE)
set(ERUI_TRANSFORMS        "FULL"         CACHE STRING "FULL 2D affine transform path"              FORCE)
set(ERUI_SCRATCH_W         "512"          CACHE STRING "Scratch buffer width in pixels"             FORCE)
set(ERUI_SCRATCH_H         "512"          CACHE STRING "Scratch buffer height in pixels"            FORCE)

add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../engine
        ${CMAKE_CURRENT_BINARY_DIR}/engine
)

add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/sdl
        ${CMAKE_CURRENT_BINARY_DIR}/backends/sdl
)

# QuickJS bridge (Flow A). The engine target already exists from above, so the bridge's
# own CMakeLists skips re-adding it. We don't need the bridge's smoke executable here.
set(ER_BRIDGE_QUICKJS_SMOKE OFF CACHE BOOL "" FORCE)
add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../bridges/quickjs
        ${CMAKE_CURRENT_BINARY_DIR}/bridges/quickjs
)

# ---------------------------------------------------------------------------
# The desktop demo — Flow A, uploaded-config model. main.c + host.c are the "firmware": they ship NO
# app and NO baked assets, and at boot load a config container (app.erpkg) from the slot next to the
# exe. This is the end-to-end "JSX on embedded" preview with only the backend swapped to SDL, and it
# mirrors how the MCU loads a separately-uploaded config (peer to examples/esp32/esp32-s3).
# ---------------------------------------------------------------------------
add_executable(embedded-react-desktop main.c host.c)

target_link_libraries(embedded-react-desktop
        PRIVATE
        er-backend-sdl
        er-bridge-quickjs
)

# Copy the packed config container into the slot next to the exe so the demo runs it by default. The
# container is produced out-of-band (`npm run pack` in bridges/quickjs/js, which bundles + bytecode-
# compiles + bakes a demo from the top-level demos/ folder into a single app.erpkg). copy_config.cmake
# no-ops when it's missing — the demo still builds and shows a "No config loaded" panel at runtime,
# just as firmware boots without a config until one is uploaded. Re-run `npm run pack` then rebuild
# (or drop a fresh app.erpkg next to the exe) to "upload" a new config.
add_custom_command(TARGET embedded-react-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DCONFIG_SRC=${CMAKE_CURRENT_SOURCE_DIR}/../../bridges/quickjs/js/dist/app.erpkg
        -DCONFIG_DST=$<TARGET_FILE_DIR:embedded-react-desktop>/app.erpkg
        -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_config.cmake
)

# On Windows, copy SDL2.dll next to the executable after build so it can be run directly from the
# build output directory (CLion, Explorer, etc.)
if (WIN32)
    add_custom_command(TARGET embedded-react-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:embedded-react-desktop>
    )
endif ()
