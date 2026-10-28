cmake_minimum_required(VERSION 3.16)
project(erb-parity C)

# ---------------------------------------------------------------------------
# erb-parity — the headless hosts the Flow B parity harness runs: one around the C output (erb-parity-c),
# one around the Flow B runtime (erb-parity-vm). Both render through the software backend, with no window,
# and share one engine build, so any difference in their frames is the compiler's or the runtime's.
#
# packages/embedded-react/test/erb-parity/erb-parity.mjs drives it: it writes the C output into
# ERB_PARITY_APP_DIR, builds, and runs both hosts. To build by hand:
#   cmake -S tools/erb-parity -B tools/erb-parity/build -DERB_PARITY_APP_DIR=<dir with app.gen.c>
#   cmake --build tools/erb-parity/build
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The desktop AOT host's engine features, so both builds draw everything the demos use.
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
set(ERUI_VECTOR_TAPE_MAX   "8192" CACHE STRING "" FORCE)
set(ERUI_VECTOR_PAINTS_MAX "1024" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_PTS    "4096" CACHE STRING "" FORCE)
set(ERUI_VECTOR_MAX_EDGES  "4096" CACHE STRING "" FORCE)
set(ER_RUNTIME_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../engine  ${CMAKE_CURRENT_BINARY_DIR}/engine)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime)

add_library(erb-parity-software STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/software/renderer_backend.c)
target_include_directories(erb-parity-software PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/software)
target_link_libraries(erb-parity-software PUBLIC embedded-react)

add_executable(erb-parity-vm host.c)
target_compile_definitions(erb-parity-vm PRIVATE ERB_PARITY_VM=1)
target_link_libraries(erb-parity-vm PRIVATE erb-runtime erb-parity-software)

# The C output's host, when there is an app to build it around.
set(ERB_PARITY_APP_DIR "" CACHE PATH "Folder holding app.gen.c, app.gen.h and assets.generated.{c,h}")
if (ERB_PARITY_APP_DIR)
    add_executable(erb-parity-c host.c "${ERB_PARITY_APP_DIR}/app.gen.c" "${ERB_PARITY_APP_DIR}/assets.generated.c")
    target_include_directories(erb-parity-c PRIVATE "${ERB_PARITY_APP_DIR}")
    target_link_libraries(erb-parity-c PRIVATE erb-parity-software)
endif ()
