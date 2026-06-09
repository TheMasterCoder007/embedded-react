# The QuickJS bridge as an ESP-IDF component — the same sources the desktop links, compiled from
# EMBEDDED_REACT_SRC_DIR (set by the top-level CMakeLists):
#   native_ui_bridge.c — the NativeUI surface over er_scene.h
#   er_runtime.c       — the portable QuickJS host core (context/globals/load/pump/reset/overlay +
#                        the ERCF config-container loader main.c uses)
#   er_assets.c        — the portable ERPK asset-pack parser (the container's asset section)
# They need er_scene.h (engine) and quickjs.h (quickjs).
set(BRIDGE_DIR "${EMBEDDED_REACT_SRC_DIR}/bridges/quickjs")

idf_component_register(
    SRCS "${BRIDGE_DIR}/native_ui_bridge.c"
         "${BRIDGE_DIR}/er_runtime.c"
         "${BRIDGE_DIR}/er_assets.c"
    INCLUDE_DIRS "${BRIDGE_DIR}"
    REQUIRES engine quickjs
)

# Same rationale as the engine component: don't let IDF's -Werror fail on the bridge's
# (warning-level) diagnostics under the stricter xtensa GCC 14.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
