# ---------------------------------------------------------------------------
# WASM simulator — Emscripten build (phase W2).
#
# Compiles the engine + backends/software + backends/web + the QuickJS bridge (er_runtime + native_ui_bridge
# + QuickJS-ng) into one app-agnostic embedded-react.{js,wasm}. It runs Flow A (QuickJS inside WASM), so a JS
# edit reloads by re-evaluating a new bundle string with no wasm rebuild — the .wasm is built once and shipped
# prebuilt in the npm package. See tools/web-sim/README.md.
#
# Build via tools/web-sim/build.mjs (which wraps emcmake), or directly:
#   emcmake cmake -S tools/web-sim -B tools/web-sim/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build tools/web-sim/build
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(embedded-react-websim C)

if (NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure with emcmake (Emscripten). See tools/web-sim/build.mjs.")
endif ()

# Capable, desktop-class engine feature set so the browser preview is faithful (matches tools/simulator).
# Every optional feature is enabled and every static cap is raised well above any single-screen UI, so the
# preview never silently disables or truncates a feature the app actually uses (e.g. conic gradients used to
# vanish when the per-node gradient pool fell back to the MCU default of 8). The per-board/device builds keep
# the lean engine defaults and tune these knobs down for their RAM/flash budget — see engine/CMakeLists.txt.
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
set(ERUI_SCRATCH_W          "1024"    CACHE STRING "" FORCE)  # opacity strip width / max transformed node width
set(ERUI_SCRATCH_H          "1024"    CACHE STRING "" FORCE)  # transform-source height (ERUI_XFORM_W/H default to these)
set(ERUI_SCRATCH_BAND_H     "32"      CACHE STRING "" FORCE)  # strip height: exercises the same banded compositing path
                                                              # boards use (banding is pixel-exact, so the preview stays faithful)
set(ERUI_FADE_CACHE_W       "1024"    CACHE STRING "" FORCE)  # composited-subtree cache for opacity animations
set(ERUI_FADE_CACHE_H       "1024"    CACHE STRING "" FORCE)  # composited-subtree cache for opacity animations

# Roomy vector pools so imported SVGs (which can have many shapes / a long op-tape) render in the browser
# preview without truncating — the desktop-class faithful preview. Tune the per-board caps separately.
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

# The QuickJS bridge brings in the engine + QuickJS-ng + er_runtime. We don't need its aux executables.
set(ER_BRIDGE_QUICKJS_SMOKE   OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_RUNTEST OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_COMPILE OFF CACHE BOOL "" FORCE)
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
# The wasm module: software compositor + web present layer/ABI, linked against the bridge. The target is
# named `websim` (the engine library already owns the `embedded-react` target name) but emits
# embedded-react.{js,wasm} via OUTPUT_NAME.
add_executable(websim
        ${ER_ROOT}/backends/software/renderer_backend.c
        ${ER_ROOT}/backends/web/renderer_backend.c
)
set_target_properties(websim PROPERTIES OUTPUT_NAME embedded-react)
target_include_directories(websim PRIVATE
        ${ER_ROOT}/backends/software
        ${ER_ROOT}/backends/web
)
target_link_libraries(websim PRIVATE er-bridge-quickjs)

# The flat C ABI the host page drives via cwrap (leading underscore = C symbol in the wasm).
set(ER_WEB_EXPORTS
        "_er_web_init,_er_web_demo_scene,_er_web_load_source,_er_web_load_pack,_er_web_compile_bytecode,_er_web_resize,_er_web_pump,_er_web_touch,_er_web_reset,_er_web_framebuffer,_er_web_fb_width,_er_web_fb_height,_malloc,_free")

target_link_options(websim PRIVATE
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createEmbeddedReact"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sSTACK_SIZE=4MB"          # headroom for QuickJS parse/eval recursion (well above its own guard)
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8"
        "-sEXPORTED_FUNCTIONS=${ER_WEB_EXPORTS}"
        # web: the simulator host page. node: the consumer `embedded-react build` loads this same module to
        # compile the app bundle to QuickJS bytecode (er_web_compile_bytecode) with no native toolchain.
        "-sENVIRONMENT=web,node"
)
