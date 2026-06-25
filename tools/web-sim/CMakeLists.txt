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
# Roomy vector pools so imported SVGs (which can have many shapes / a long op-tape) render in the browser
# preview without truncating — the desktop-class faithful preview. Tune the per-board caps separately.
set(ERUI_VECTOR_TAPE_MAX   "8192" CACHE STRING "" FORCE)
set(ERUI_VECTOR_PAINTS_MAX "1024" CACHE STRING "" FORCE)  # a designer dial face is ~100 shapes = ~700 paints
set(ERUI_VECTOR_MAX_PTS    "4096" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_EDGES  "4096" CACHE STRING "" FORCE)

set(ER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# The QuickJS bridge brings in the engine + QuickJS-ng + er_runtime. We don't need its aux executables.
set(ER_BRIDGE_QUICKJS_SMOKE   OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_RUNTEST OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_COMPILE OFF CACHE BOOL "" FORCE)
add_subdirectory(${ER_ROOT}/bridges/quickjs ${CMAKE_CURRENT_BINARY_DIR}/bridges/quickjs)

# Match the engine's raised vector caps on the bridge side (it copies the JS op-tape into a fixed buffer
# before handing it to the engine, so its caps must be >= the engine's or it truncates large SVGs first).
target_compile_definitions(er-bridge-quickjs PRIVATE VEC_BRIDGE_MAX_OPS=8192 VEC_BRIDGE_MAX_PAINTS=1024 VEC_BRIDGE_MAX_GRADS=${ERUI_VECTOR_GRADS_MAX})
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
