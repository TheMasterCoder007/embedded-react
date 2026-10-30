# The Flow B runtime as an ESP-IDF component: it checks a package and runs it, with no QuickJS and no heap.
# Compiled from EMBEDDED_REACT_SRC_DIR (set by the top-level CMakeLists), and only in the Flow B build, where
# er-bridge-quickjs compiles nothing. The Flow A build carries none of it, and gets the container reader and asset
# parser from er-bridge-quickjs.
set(R "${EMBEDDED_REACT_SRC_DIR}/runtime")

set(erb_srcs "")
if(CONFIG_ER_FLOW_B)
    set(erb_srcs
        "${R}/er_container.c"
        "${R}/er_assets.c"
        "${R}/erb_props.c"
        "${R}/erb_verify.c"
        "${R}/erb_vm.c"
        "${R}/erb_runtime.c"
        "${R}/erb_package_slots.c"
        "${R}/port/erb_package_slots_esp32.c")
endif()

idf_component_register(
    SRCS ${erb_srcs}
    INCLUDE_DIRS "${R}" "${R}/port"
    REQUIRES engine esp_partition spi_flash
)

if(CONFIG_ER_FLOW_B)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
endif()
