cmake_minimum_required(VERSION 3.16)
project(embedded-react-simulator C)

# ---------------------------------------------------------------------------
# embedded-react simulator — the dev tool (see /SIMULATOR.md).
#
# The same SDL + QuickJS + engine host as the desktop demo: it reuses the shared host core
# (examples/linux/host.c) and adds a watch + live-reload loop (sim_main.c). The demo
# (examples/linux) stays a plain demo; this is the dedicated dev simulator, launched by
# `npm run sim` (bridges/quickjs/js), which runs esbuild --watch to rewrite the bundle on save.
#
# Configure + build once:
#   cmake -S tools/simulator -B tools/simulator/build [-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake]
#   cmake --build tools/simulator/build
# Then iterate with `npm run sim`.
# ---------------------------------------------------------------------------

find_package(SDL2 REQUIRED)

# Match the desktop demo's engine feature set (full transforms/shadows/gradient, big scratch buffer)
# so the simulator renders exactly what the demo host does.
set(ERUI_SHADOWS         ON     CACHE BOOL   "" FORCE)
set(ERUI_BORDER_AA       ON     CACHE BOOL   "" FORCE)
set(ERUI_3D_TRANSFORMS   ON     CACHE BOOL   "" FORCE)
set(ERUI_BILINEAR_SCALE  ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT        ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT_RADIAL ON     CACHE BOOL   "" FORCE)
set(ERUI_TRANSFORMS      "FULL" CACHE STRING "" FORCE)
set(ERUI_SCRATCH_W       "512"  CACHE STRING "" FORCE)
set(ERUI_SCRATCH_H       "512"  CACHE STRING "" FORCE)

set(ER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_subdirectory(${ER_ROOT}/engine ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${ER_ROOT}/backends/sdl ${CMAKE_CURRENT_BINARY_DIR}/backends/sdl)

# QuickJS bridge (Flow A); the engine target already exists from above, so its CMakeLists skips
# re-adding it. We don't need the bridge's smoke executable here.
set(ER_BRIDGE_QUICKJS_SMOKE OFF CACHE BOOL "" FORCE)
add_subdirectory(${ER_ROOT}/bridges/quickjs ${CMAKE_CURRENT_BINARY_DIR}/bridges/quickjs)

# The shared desktop host core lives with the demo; the simulator reuses it (sim_main.c is just the
# watch + reload loop over it).
set(ER_HOST_DIR "${ER_ROOT}/examples/linux")
set(ER_GENERATED_ASSETS "${ER_ROOT}/bridges/quickjs/js/dist/assets.generated.c")

# Baked assets (images + fonts) are compiled in like the demo, so they resolve while JS hot-reloads.
# (Asset *changes* need a sim rebuild — runtime asset reload is Phase 2 in /SIMULATOR.md.)
if (EXISTS "${ER_GENERATED_ASSETS}")
    add_executable(embedded-react-simulator sim_main.c "${ER_HOST_DIR}/host.c" "${ER_GENERATED_ASSETS}")
    target_include_directories(embedded-react-simulator PRIVATE
            "${ER_HOST_DIR}" "${ER_ROOT}/bridges/quickjs/js/dist")
    target_compile_definitions(embedded-react-simulator PRIVATE ER_HAVE_BAKED_ASSETS=1)
else ()
    add_executable(embedded-react-simulator sim_main.c "${ER_HOST_DIR}/host.c")
    target_include_directories(embedded-react-simulator PRIVATE "${ER_HOST_DIR}")
    message(STATUS "embedded-react-simulator: no baked assets at ${ER_GENERATED_ASSETS} "
            "(run `npm run build` in bridges/quickjs/js, then reconfigure)")
endif ()

target_link_libraries(embedded-react-simulator
        PRIVATE
        er-backend-sdl
        er-bridge-quickjs
)

# On Windows, copy SDL2.dll next to the executable so it runs from the build dir.
if (WIN32)
    add_custom_command(TARGET embedded-react-simulator POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:embedded-react-simulator>
    )
endif ()
