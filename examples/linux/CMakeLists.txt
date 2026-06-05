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
# Two desktop targets share the engine + SDL backend:
#   embedded-react-desktop     — the hand-written C engine showcase (main.c)
#   embedded-react-desktop-js  — Flow A: a JS app drives the engine via QuickJS (main_js.c)
# ---------------------------------------------------------------------------
add_executable(embedded-react-desktop main.c)
target_link_libraries(embedded-react-desktop
        PRIVATE
        er-backend-sdl
)

add_executable(embedded-react-desktop-js main_js.c)
target_link_libraries(embedded-react-desktop-js
        PRIVATE
        er-backend-sdl
        er-bridge-quickjs
)

# Copy the React bundle next to the desktop-js executable so it runs by default (no CLI arg).
# The bundle is built out-of-band (`npm run build` in bridges/quickjs/js, which bundles a demo from
# the top-level demos/ folder); if it's absent the example falls back to its built-in demo.
# copy_bundle.cmake no-ops when the bundle is missing.
add_custom_command(TARGET embedded-react-desktop-js POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DBUNDLE_SRC=${CMAKE_CURRENT_SOURCE_DIR}/../../bridges/quickjs/js/dist/app.bundle.js
        -DBUNDLE_DST=$<TARGET_FILE_DIR:embedded-react-desktop-js>/app.bundle.js
        -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_bundle.cmake
)

# Also precompile the bundle to a QuickJS bytecode blob (app.bundle.qbc) next to the exe. The host
# prefers it over the source bundle — the same JS_ReadObject load path the MCU hosts use (Flow A;
# QuickJS still runs it). Requires the bytecode precompiler tool, so depend on it to build it first.
add_dependencies(embedded-react-desktop-js er-bridge-quickjs-compile)
add_custom_command(TARGET embedded-react-desktop-js POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DCOMPILE_EXE=$<TARGET_FILE:er-bridge-quickjs-compile>
        -DBUNDLE_SRC=${CMAKE_CURRENT_SOURCE_DIR}/../../bridges/quickjs/js/dist/app.bundle.js
        -DBYTECODE_DST=$<TARGET_FILE_DIR:embedded-react-desktop-js>/app.bundle.qbc
        -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_bytecode.cmake
)

# On Windows, copy SDL2.dll next to each executable after build so the binaries can be
# run directly from the build output directory (CLion, Explorer, etc.)
if (WIN32)
    foreach (er_desktop_target embedded-react-desktop embedded-react-desktop-js)
        add_custom_command(TARGET ${er_desktop_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL2::SDL2>
                $<TARGET_FILE_DIR:${er_desktop_target}>
        )
    endforeach ()
endif ()
