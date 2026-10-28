# The QuickJS bridge as an ESP-IDF component — the same sources the desktop links, compiled from
# EMBEDDED_REACT_SRC_DIR (set by the top-level CMakeLists):
#   native_ui_bridge.c — the NativeUI surface over er_scene.h
#   er_runtime.c       — the portable QuickJS host core (context/globals/load/pump/reset/overlay +
#                        the config-container loader main.c uses, and the hot-reload apply step)
#   er_js_alloc.c      — default JS-heap allocator with working GC size accounting (unused here: main.c
#                        supplies its own PSRAM allocator, but er_runtime's boot-time probe lives with it)
# plus the parts from runtime/ that no JS engine owns:
#   er_container.c     — the ERCF container reader and CRC
#   er_assets.c        — the portable ERPK asset-pack parser (the container's asset section)
#   er_assets_heap.c   — its heap-backed loader (er_assets_load_pack)
#   er_hotreload.c     — the portable ERHR transport-frame parser for on-device hot reload (the RX task
#                        in hotreload_usb.c feeds received USB/serial bytes into it)
# They need er_scene.h (engine) and quickjs.h (quickjs).
set(BRIDGE_DIR "${EMBEDDED_REACT_SRC_DIR}/bridges/quickjs")
set(RUNTIME_DIR "${EMBEDDED_REACT_SRC_DIR}/runtime")

idf_component_register(
    SRCS "${BRIDGE_DIR}/native_ui_bridge.c"
         "${BRIDGE_DIR}/er_runtime.c"
         "${BRIDGE_DIR}/er_js_alloc.c"
         "${BRIDGE_DIR}/overlay/message_overlay.qbc.c" # precompiled error overlay (parser-less safe)
         "${RUNTIME_DIR}/er_container.c"
         "${RUNTIME_DIR}/er_assets.c"
         "${RUNTIME_DIR}/er_assets_heap.c"
         "${RUNTIME_DIR}/er_hotreload.c"
    INCLUDE_DIRS "${BRIDGE_DIR}" "${RUNTIME_DIR}"
    REQUIRES engine quickjs
    LDFRAGMENTS "linker_psram.lf" # the bridge's tables live in PSRAM (see the file)
)

# Same rationale as the engine component: don't let IDF's -Werror fail on the bridge's
# (warning-level) diagnostics under the stricter xtensa GCC 14.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)

# The bridge marks the JS phase's own split (dispatch / reconcile / marshalling / commit, er_perf.h),
# and those marks compile out unless ER_PERF_STATS reaches THIS component too — the engine component
# sets it only on itself. Same ER_PERF_DETAIL switch, so `idf.py -DER_PERF_DETAIL=1` lights up the
# whole frame split at once and the default build carries none of it.
if(NOT DEFINED ER_PERF_DETAIL)
    set(ER_PERF_DETAIL 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_STATS=${ER_PERF_DETAIL})
