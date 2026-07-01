cmake_minimum_required(VERSION 3.16)
project(embedded-react-quickjs-bridge C)

# ---------------------------------------------------------------------------
# er-bridge-quickjs — QuickJS host config bridge (Flow A)
#
# Publishes the NativeUI surface into a QuickJS context and forwards it to the
# engine's er_scene.h API. QuickJS-ng is pulled in via FetchContent, pinned to
# a release tag.
#
# Configure standalone (link smoke test):
#   cmake -S bridges/quickjs -B build-bridge -G "MinGW Makefiles"
#   cmake --build build-bridge
#   ./build-bridge/er-bridge-quickjs-smoke
#
# Or add_subdirectory() this folder from an example that already defines the
# embedded-react engine target.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# When configured standalone (not add_subdirectory'd by an example), pull in the
# engine so the embedded-react target exists for linking.
if (NOT TARGET embedded-react)
    add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../../engine
            ${CMAKE_CURRENT_BINARY_DIR}/engine
    )
endif ()

# ---------------------------------------------------------------------------
# QuickJS-ng — pinned release. Keep the upstream build lean: we only need the
# library, not its CLI, examples, or test262 harness.
# ---------------------------------------------------------------------------
include(FetchContent)

set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        quickjs
        GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
        GIT_TAG        v0.15.0
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(quickjs)

# ---------------------------------------------------------------------------
# QuickJS-ng defines its CLI + test executables unconditionally (no upstream option
# disables them). We only need the `qjs` library. Drop the rest from the default build
# so a normal build doesn't compile them and the IDE doesn't surface them as targets.
# EXCLUDE_FROM_ALL keeps each target buildable on demand (e.g. `--target qjsc` when §4
# needs the bytecode compiler) — it just removes them from the `all` target.
# ---------------------------------------------------------------------------
foreach (qjs_aux qjs_exe qjsc run-test262 api-test lre-test function_source unicode_gen)
    if (TARGET ${qjs_aux})
        set_target_properties(${qjs_aux} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif ()
endforeach ()

# ---------------------------------------------------------------------------
# Bridge library — thin adapter over er_scene.h (native_ui_bridge.c), plus the portable QuickJS host
# core (er_runtime.c: context lifecycle, console/screen/persist globals, load/pump/reset/error) and the
# portable hot-reload receiver (er_hotreload.c: the ERHR transport-frame parser firmware feeds bytes
# into to swap the running config over USB/serial). Hosts (desktop, MCU firmware) build on er_runtime
# with a few lines of glue — see er_runtime.h.
# ---------------------------------------------------------------------------
add_library(er-bridge-quickjs STATIC native_ui_bridge.c er_runtime.c er_assets.c er_hotreload.c)

target_link_libraries(er-bridge-quickjs
        PUBLIC
        embedded-react
        qjs
)

target_include_directories(er-bridge-quickjs
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# Smoke-test executable — the §0 vertical-slice link check. Proves QuickJS
# compiles, links against the engine, runs JS, and reaches the NativeUI global.
# ---------------------------------------------------------------------------
option(ER_BRIDGE_QUICKJS_SMOKE "Build the QuickJS bridge smoke-test executable" ON)

if (ER_BRIDGE_QUICKJS_SMOKE)
    add_executable(er-bridge-quickjs-smoke smoke_main.c)
    target_link_libraries(er-bridge-quickjs-smoke PRIVATE er-bridge-quickjs)
endif ()

# ---------------------------------------------------------------------------
# Headless runtime-test harness — loads a bundled JS test (esbuild output) and runs it under the
# bridge + engine with a no-op backend, no window. Driven by `npm run test:runtime` in js/.
# ---------------------------------------------------------------------------
option(ER_BRIDGE_QUICKJS_RUNTEST "Build the headless runtime-test harness" ON)

if (ER_BRIDGE_QUICKJS_RUNTEST)
    add_executable(er-bridge-quickjs-runtest runtest_main.c)
    target_link_libraries(er-bridge-quickjs-runtest PRIVATE er-bridge-quickjs)
endif ()

# ---------------------------------------------------------------------------
# Split round-trip harness — loads a vendor/app SPLIT boot container then an app-only frame (no reset),
# verifying the resident vendor is reused and usePersistentState survives. The desktop proof of the device
# hot-reload mechanism. Driven by `node test/runtime/split-roundtrip.mjs` in js/.
# ---------------------------------------------------------------------------
option(ER_BRIDGE_QUICKJS_SPLITTEST "Build the vendor/app split round-trip harness" ON)

if (ER_BRIDGE_QUICKJS_SPLITTEST)
    add_executable(er-bridge-quickjs-splittest splittest_main.c)
    target_link_libraries(er-bridge-quickjs-splittest PRIVATE er-bridge-quickjs)
endif ()

# ---------------------------------------------------------------------------
# Bytecode precompiler — turns the esbuild bundle into a QuickJS bytecode blob (or a C array for
# embedding in MCU flash). Links only `qjs` (no engine/bridge needed to compile). Hosts load the
# blob with JS_ReadObject instead of parsing source — the §4 boot/RAM lever for ESP32-S3 / STM32H7.
# NOTE: this is a parse-time optimization for Flow A; the QuickJS VM still runs the bytecode. It is
# NOT the Flow B AOT compiler (which compiles JSX to C and removes QuickJS) — see js/aot/.
# ---------------------------------------------------------------------------
option(ER_BRIDGE_QUICKJS_COMPILE "Build the QuickJS bytecode precompiler tool" ON)

if (ER_BRIDGE_QUICKJS_COMPILE)
    add_executable(er-bridge-quickjs-compile compile_main.c)
    target_link_libraries(er-bridge-quickjs-compile PRIVATE qjs)
    target_include_directories(er-bridge-quickjs-compile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif ()
