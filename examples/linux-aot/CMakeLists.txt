cmake_minimum_required(VERSION 3.16)
project(embedded-react-desktop-aot C)

# ---------------------------------------------------------------------------
# embedded-react-desktop-aot — Flow B (AOT) desktop demo.
#
# Renders an AOT-compiled JSX app (dist/app.gen.c from `npm run aot`) in an SDL2 window with NO QuickJS
# and no JS at runtime — the desktop proxy for a no-PSRAM MCU. Links only the engine + SDL backend +
# the generated C; the QuickJS bridge is deliberately absent.
#
# Generate the app first, then build:
#   (in bridges/quickjs/js)  npm run aot -- music-player   # → dist/app.gen.{c,h}
#   cmake -S examples/linux-aot -B examples/linux-aot/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/...vcpkg.cmake]
#   cmake --build examples/linux-aot/build
# ---------------------------------------------------------------------------

find_package(SDL2 REQUIRED)

# Match the desktop demo's engine feature set so AOT renders exactly what Flow A does.
set(ERUI_SHADOWS         ON     CACHE BOOL   "" FORCE)
set(ERUI_BORDER_AA       ON     CACHE BOOL   "" FORCE)
set(ERUI_3D_TRANSFORMS   ON     CACHE BOOL   "" FORCE)
set(ERUI_BILINEAR_SCALE  ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT        ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT_RADIAL ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT_CONIC  ON     CACHE BOOL   "" FORCE)
set(ERUI_ONSCREEN_KEYBOARD ON   CACHE BOOL   "" FORCE)
set(ERUI_TRANSFORMS      "FULL" CACHE STRING "" FORCE)
set(ERUI_SCRATCH_W       "512"  CACHE STRING "" FORCE)
set(ERUI_SCRATCH_H       "512"  CACHE STRING "" FORCE)
# Roomy vector pools so a baked <Svg source> (a designer dial = many flattened arc points) renders without
# truncating — matches tools/simulator + tools/web-sim.
set(ERUI_VECTOR_TAPE_MAX   "8192" CACHE STRING "" FORCE)
set(ERUI_VECTOR_PAINTS_MAX "1024" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_PTS    "4096" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_EDGES  "4096" CACHE STRING "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine       ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../backends/sdl ${CMAKE_CURRENT_BINARY_DIR}/backends/sdl)

# The AOT-generated app translation unit — produced out-of-band by `npm run aot` into
# bridges/quickjs/js/dist. Like Flow A's bundle it's a generated artifact (not committed); fail with a
# clear message if it hasn't been generated yet.
set(ER_APP_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../bridges/quickjs/js/dist")
if (NOT EXISTS "${ER_APP_GEN_DIR}/app.gen.c")
    message(FATAL_ERROR
            "No AOT app at ${ER_APP_GEN_DIR}/app.gen.c — generate it first:\n"
            "    cd bridges/quickjs/js && npm run aot -- <demo>   (e.g. music-player)")
endif ()

# app.gen.c + assets.generated.c are produced together by `npm run aot`: the app tree and the baked-image
# table (er_register_assets, called by main.c before er_app_build). assets.generated.c is always written —
# a no-op register fn when the demo imports no images — so it is always safe to link + call.
add_executable(embedded-react-desktop-aot main.c "${ER_APP_GEN_DIR}/app.gen.c" "${ER_APP_GEN_DIR}/assets.generated.c")
target_include_directories(embedded-react-desktop-aot PRIVATE "${ER_APP_GEN_DIR}")
target_link_libraries(embedded-react-desktop-aot PRIVATE er-backend-sdl embedded-react)

# On Windows, copy SDL2.dll next to the exe so it runs from the build dir.
if (WIN32)
    add_custom_command(TARGET embedded-react-desktop-aot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:embedded-react-desktop-aot>)
endif ()
