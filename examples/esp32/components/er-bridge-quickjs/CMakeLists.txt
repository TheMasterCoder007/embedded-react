# The QuickJS NativeUI bridge as an ESP-IDF component — the same single native_ui_bridge.c the
# desktop links, compiled from EMBEDDED_REACT_SRC_DIR (set by the top-level CMakeLists). It only
# needs er_scene.h (engine) and quickjs.h (quickjs).
set(BRIDGE_DIR "${EMBEDDED_REACT_SRC_DIR}/bridges/quickjs")

idf_component_register(
    SRCS "${BRIDGE_DIR}/native_ui_bridge.c"
    INCLUDE_DIRS "${BRIDGE_DIR}"
    REQUIRES engine quickjs
)

# Same rationale as the engine component: don't let IDF's -Werror fail on the bridge's
# (warning-level) diagnostics under the stricter xtensa GCC 14.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
