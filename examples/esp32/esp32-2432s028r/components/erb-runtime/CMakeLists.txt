# The Flow B runtime as an ESP-IDF component: it checks a package and runs it, with no QuickJS and no heap.
# Compiled from EMBEDDED_REACT_SRC_DIR (set by the top-level CMakeLists).
set(R "${EMBEDDED_REACT_SRC_DIR}/runtime")

idf_component_register(
    SRCS
        "${R}/er_container.c"
        "${R}/er_assets.c"
        "${R}/erb_props.c"
        "${R}/erb_verify.c"
        "${R}/erb_vm.c"
        "${R}/erb_runtime.c"
        "${R}/erb_package_slots.c"
        "${R}/port/erb_package_slots_esp32.c"
    INCLUDE_DIRS "${R}" "${R}/port"
    REQUIRES engine esp_partition spi_flash
)

# How large an app this firmware accepts: most of these size the verifier's fixed scratch, in a board whose DRAM
# is nearly full. PUBLIC, since erb_internal.h sizes structs by them.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ERB_MAX_FUNCTIONS=64
    ERB_MAX_FUNCTION_WORDS=4096
    ERB_MAX_KEYBOARD_KEYS=64
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
