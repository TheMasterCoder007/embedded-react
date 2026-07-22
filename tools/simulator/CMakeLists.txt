cmake_minimum_required(VERSION 3.16)
project(embedded-react-simulator C)

# ---------------------------------------------------------------------------
# embedded-react simulator — the dev tool (see tools/simulator/README.md).
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
# so the simulator renders exactly what the demo host does. Every optional feature is enabled and every
# static cap is raised well above any single-screen UI, so the simulator never silently disables or
# truncates a feature the app actually uses (e.g. conic gradients used to vanish when the per-node gradient
# pool fell back to the MCU default of 8). Mirrors tools/web-sim; per-board/device builds tune these down.
set(ERUI_SHADOWS         ON     CACHE BOOL   "" FORCE)
set(ERUI_BORDER_AA       ON     CACHE BOOL   "" FORCE)
set(ERUI_3D_TRANSFORMS   ON     CACHE BOOL   "" FORCE)
set(ERUI_BILINEAR_SCALE  ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT        ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT_RADIAL ON     CACHE BOOL   "" FORCE)
set(ERUI_GRADIENT_CONIC  ON     CACHE BOOL   "" FORCE)
set(ERUI_ONSCREEN_KEYBOARD ON   CACHE BOOL   "" FORCE)
set(ERUI_TRANSFORMS      "FULL" CACHE STRING "" FORCE)

# Scene / compositor / animation pools — generous so large screens and busy UIs render fully.
set(ERUI_MAX_NODES          "4096"    CACHE STRING "" FORCE)
set(ERUI_MAX_OPACITY_DEPTH  "16"      CACHE STRING "" FORCE)
set(ERUI_MAX_ANIM_VALUES    "256"     CACHE STRING "" FORCE)
set(ERUI_MAX_VALUE_BINDINGS "16"      CACHE STRING "" FORCE)
set(ERUI_FONT_POOL_BYTES    "1048576" CACHE STRING "" FORCE)  # enable runtime er_font_load (0 disables it)
set(ERUI_MAX_IMG_ROW_PIXELS "4096"    CACHE STRING "" FORCE)
set(ERUI_SCRATCH_W          "1024"    CACHE STRING "" FORCE)  # max compositable / transformed node size
set(ERUI_SCRATCH_H          "1024"    CACHE STRING "" FORCE)
set(ERUI_FADE_CACHE_W       "1024"    CACHE STRING "" FORCE)  # composited-subtree cache for opacity animations
set(ERUI_FADE_CACHE_H       "1024"    CACHE STRING "" FORCE)

# Roomy vector pools so imported SVGs (many shapes / a long op-tape) render in the preview without
# truncating — matches tools/web-sim. Per-board caps are tuned separately.
set(ERUI_VECTOR_TAPE_MAX     "8192" CACHE STRING "" FORCE)
set(ERUI_VECTOR_PAINTS_MAX   "1024" CACHE STRING "" FORCE)  # a designer dial face is ~100 shapes = ~700 paints
set(ERUI_VECTOR_GRADS_MAX    "256"  CACHE STRING "" FORCE)  # gradient entries per node (linear/radial/conic)
set(ERUI_VECTOR_GRAD_LUT     "1024" CACHE STRING "" FORCE)  # gradient colour-LUT entries (smoother ramps)
set(ERUI_VECTOR_MAX_PTS      "4096" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_SUBPATHS "1024" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_EDGES    "4096" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_ROW      "4096" CACHE STRING "" FORCE)  # max vector-node width in px
set(ERUI_MAX_VECTOR_NODES    "64"   CACHE STRING "" FORCE)  # concurrent vector nodes holding geometry

set(ER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_subdirectory(${ER_ROOT}/engine ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${ER_ROOT}/backends/sdl ${CMAKE_CURRENT_BINARY_DIR}/backends/sdl)

# QuickJS bridge (Flow A); the engine target already exists from above, so its CMakeLists skips
# re-adding it. We don't need the bridge's smoke executable here.
set(ER_BRIDGE_QUICKJS_SMOKE OFF CACHE BOOL "" FORCE)
add_subdirectory(${ER_ROOT}/bridges/quickjs ${CMAKE_CURRENT_BINARY_DIR}/bridges/quickjs)

# Match the engine's raised caps on the bridge side. The bridge keeps its OWN fixed-size pools (node-handle
# table, animation-completion slots, timers, and a copy of the JS vector op-tape) that gate JS-side creation
# independently of the engine, so each must be >= its engine counterpart or the simulator silently caps below
# the engine limit (e.g. handles < ERUI_MAX_NODES truncates node creation; completions < ERUI_MAX_ANIM_VALUES
# drops callbacks; a small op-tape buffer truncates large SVGs first).
target_compile_definitions(er-bridge-quickjs PRIVATE
        ER_BRIDGE_MAX_HANDLES=8192            # >= ERUI_MAX_NODES (4096)
        ER_BRIDGE_MAX_ANIM_COMPLETIONS=256    # >= ERUI_MAX_ANIM_VALUES (256)
        ER_BRIDGE_MAX_TIMERS=256
        VEC_BRIDGE_MAX_OPS=8192               # >= ERUI_VECTOR_TAPE_MAX
        VEC_BRIDGE_MAX_PAINTS=1024            # >= ERUI_VECTOR_PAINTS_MAX
        VEC_BRIDGE_MAX_GRADS=${ERUI_VECTOR_GRADS_MAX})
# The shared desktop host core lives with the demo; the simulator reuses it (sim_main.c is just the
# watch + reload loop over it). Unlike the demo/device, the simulator does NOT compile assets in: it
# loads them at runtime from a binary pack (asset_pack.c) so images/fonts hot-reload (Phase 2b).
set(ER_HOST_DIR "${ER_ROOT}/examples/linux")

add_executable(embedded-react-simulator sim_main.c asset_pack.c "${ER_HOST_DIR}/host.c")
target_include_directories(embedded-react-simulator PRIVATE "${ER_HOST_DIR}")

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
