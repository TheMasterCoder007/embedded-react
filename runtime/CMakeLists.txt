cmake_minimum_required(VERSION 3.16)
project(embedded-react-runtime C)

# ---------------------------------------------------------------------------
# er-runtime — the parts of the device runtime that no JS engine owns: the ERCF container reader and
# CRC (er_container.c), the ERPK asset-pack loader (er_assets.c) and the hot-reload frame parser
# (er_hotreload.c). Links only the engine. The QuickJS bridge builds on it; so will the Flow B runtime.
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
        er_hotreload.c
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
endif ()
