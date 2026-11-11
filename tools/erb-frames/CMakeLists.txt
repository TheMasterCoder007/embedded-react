cmake_minimum_required(VERSION 3.16)
project(erb-frames C)

# ---------------------------------------------------------------------------
# erb-frames — the headless host the frame test runs: the Flow B runtime, and Flow A's QuickJS runtime for a bundle,
# rendering through the software backend with no window.
#
# packages/embedded-react/test/erb-frames/erb-frames.mts drives it. To build by hand:
#   cmake -S tools/erb-frames -B tools/erb-frames/build
#   cmake --build tools/erb-frames/build
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/engine-features.cmake)
set(ER_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)

# The golden images are recorded on one machine and checked on others, so a multiply-add can't be fused on some.
if (NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif ()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine  ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)

# Flow A: the QuickJS bridge, without its own tools and tests.
foreach (bridge_tool SMOKE RUNTEST SPLITTEST CONTAINERTEST HANDLETEST ERRORTEST NATIVETEST GCTEST COMPILE)
    set(ER_BRIDGE_QUICKJS_${bridge_tool} OFF CACHE BOOL "" FORCE)
endforeach ()
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../bridges/quickjs ${CMAKE_CURRENT_BINARY_DIR}/bridges/quickjs)
# QuickJS is third-party code, which CI's -Werror=shadow isn't for.
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qjs PRIVATE -Wno-shadow)
endif ()

# A rename that collides with a local can still compile and pass the plain build, so a shadow has to warn.
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wshadow)
endif ()

add_executable(erb-frames host.c ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/software/renderer_backend.c)
target_include_directories(erb-frames PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/software)
target_link_libraries(erb-frames PRIVATE erb-runtime er-bridge-quickjs embedded-react)
