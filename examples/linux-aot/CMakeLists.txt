cmake_minimum_required(VERSION 3.16)
project(embedded-react-desktop-aot C)

# ---------------------------------------------------------------------------
# embedded-react-desktop-aot — Flow B (AOT) desktop demo.
#
# Runs a Flow B package (dist/app.erpkg from `npm run aot`) in an SDL2 window with NO QuickJS and no JS
# at runtime — the desktop proxy for a no-PSRAM MCU. Links only the engine + SDL backend + the Flow B
# runtime; the QuickJS bridge is deliberately absent.
#
#   cmake -S examples/linux-aot -B examples/linux-aot/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/...vcpkg.cmake]
#   cmake --build examples/linux-aot/build
#   (in packages/embedded-react)  npm run aot -- watch-face   # → dist/app.erpkg
#   examples/linux-aot/build/embedded-react-desktop-aot [package.erpkg]
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
set(ERUI_SCRATCH_WIDTH   "512"  CACHE STRING "" FORCE)
set(ERUI_SCRATCH_HEIGHT  "512"  CACHE STRING "" FORCE)
# Roomy vector pools so a baked <Svg source> (a designer dial = many flattened arc points) renders without
# truncating — matches tools/simulator + tools/web-sim.
set(ERUI_VECTOR_TAPE_MAX   "8192" CACHE STRING "" FORCE)
set(ERUI_VECTOR_PAINTS_MAX "1024" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_POINTS "4096" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_EDGES  "4096" CACHE STRING "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine       ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../backends/sdl ${CMAKE_CURRENT_BINARY_DIR}/backends/sdl)

set(ER_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)

# The package is read at run time, so changing the app needs no rebuild.
# The native modules are the Flow A desktop host's, so both flows call the same C.
add_executable(embedded-react-desktop-aot main.c ../linux/desktop_natives.c)
target_include_directories(embedded-react-desktop-aot PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../linux
        ${CMAKE_CURRENT_SOURCE_DIR}/../device/include) # the device package's generated headers
target_compile_definitions(embedded-react-desktop-aot PRIVATE
        ER_DEFAULT_PACKAGE="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/embedded-react/dist/app.erpkg")
target_link_libraries(embedded-react-desktop-aot PRIVATE er-backend-sdl erb-runtime)

# On Windows, copy SDL2.dll next to the exe so it runs from the build dir.
if (WIN32)
    add_custom_command(TARGET embedded-react-desktop-aot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:embedded-react-desktop-aot>)
endif ()
