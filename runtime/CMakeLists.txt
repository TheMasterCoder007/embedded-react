cmake_minimum_required(VERSION 3.16)
project(embedded-react-runtime C)

# ---------------------------------------------------------------------------
# er-runtime — the parts of the device runtime that no JS engine owns: the ERCF container reader and
# CRC (er_container.c), the ERPK asset-pack loader (er_assets.c, with its heap path in er_assets_heap.c)
# and the hot-reload frame parser (er_hotreload.c). Links only the engine. The QuickJS bridge builds on it; so will the Flow B runtime.
#
# Configure standalone (runs the container tests):
#   cmake -S runtime -B runtime/build
#   cmake --build runtime/build
#   ctest --test-dir runtime/build --output-on-failure
#
# Or add_subdirectory() this folder from a project that already defines the embedded-react target.
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (NOT TARGET embedded-react)
    add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../engine
            ${CMAKE_CURRENT_BINARY_DIR}/engine
    )
endif ()

add_library(er-runtime STATIC
        er_container.c
        er_assets.c
        er_assets_heap.c
        er_hotreload.c
)

# The Flow B runtime: loader, verifier and VM for compiled packages. No QuickJS, no allocation.
add_library(erb-runtime STATIC
        erb_props.c
        erb_verify.c
        erb_vm.c
        erb_runtime.c
)

# Flow B limits: how large an app this firmware accepts. Most size the verifier's fixed scratch, and a package
# past one is rejected with the reason. The engine's pool sizes are its own ERUI_* options, which the runtime
# reads at load time; its image index follows ERUI_IMAGE_REGISTRY_MAX.
set(ERB_EVENT_BUDGET "100000" CACHE STRING "Flow B: instructions one run of an entry point may execute (ErbConfig::budget overrides it)")
set(ERB_MAX_FUNCTIONS "256" CACHE STRING "Flow B: functions per package (8 B of verifier scratch each)")
set(ERB_MAX_FUNCTION_WORDS "16384" CACHE STRING "Flow B: instruction words per function (1 bit of verifier scratch each)")
set(ERB_MAX_LOOPS "32" CACHE STRING "Flow B: loops per function")
set(ERB_MAX_IMPORTS "32" CACHE STRING "Flow B: native imports per package")
set(ERB_MAX_TEMPLATE_DEPTH "32" CACHE STRING "Flow B: how deeply a template may nest")
set(ERB_REPORT_PLACES "32" CACHE STRING "Flow B: places a limit is reported from before repeats stop being filtered")

# PUBLIC: erb_internal.h sizes structs by these, so every file that includes it must agree.
target_compile_definitions(erb-runtime PUBLIC
        ERB_EVENT_BUDGET=${ERB_EVENT_BUDGET}
        ERB_MAX_FUNCTIONS=${ERB_MAX_FUNCTIONS}
        ERB_MAX_FUNCTION_WORDS=${ERB_MAX_FUNCTION_WORDS}
        ERB_MAX_LOOPS=${ERB_MAX_LOOPS}
        ERB_MAX_IMPORTS=${ERB_MAX_IMPORTS}
        ERB_MAX_TEMPLATE_DEPTH=${ERB_MAX_TEMPLATE_DEPTH}
        ERB_REPORT_PLACES=${ERB_REPORT_PLACES}
)

target_link_libraries(erb-runtime
        PUBLIC
        er-runtime
)

target_link_libraries(er-runtime
        PUBLIC
        embedded-react
)

target_include_directories(er-runtime
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# Tests — on by default when this folder is the top-level project. A parent that wants them run by its
# own ctest (the QuickJS bridge does) sets ER_RUNTIME_TESTS and calls enable_testing() first.
# ---------------------------------------------------------------------------
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_er_runtime_top_level ON)
else ()
    set(_er_runtime_top_level OFF)
endif ()
option(ER_RUNTIME_TESTS "Build and register the runtime tests" ${_er_runtime_top_level})

if (ER_RUNTIME_TESTS)
    enable_testing()
    add_executable(er-runtime-container-test tests/container_test.c)
    target_link_libraries(er-runtime-container-test PRIVATE er-runtime)
    add_test(NAME runtime-container COMMAND er-runtime-container-test)

    add_executable(er-runtime-assets-test tests/assets_test.c)
    target_link_libraries(er-runtime-assets-test PRIVATE er-runtime)
    add_test(NAME runtime-assets COMMAND er-runtime-assets-test)

    add_executable(erb-verify-test tests/erb_verify_test.c)
    target_link_libraries(erb-verify-test PRIVATE erb-runtime)
    add_test(NAME runtime-erb-verify COMMAND erb-verify-test)

    # The VM tests fire events the way the engine does, so they see its node internals.
    add_executable(erb-vm-test tests/erb_vm_test.c)
    target_link_libraries(erb-vm-test PRIVATE erb-runtime)
    target_include_directories(erb-vm-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../engine/scene)
    add_test(NAME runtime-erb-vm COMMAND erb-vm-test)
endif ()
