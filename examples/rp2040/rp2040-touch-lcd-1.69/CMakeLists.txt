# pico-sdk project for the embedded-react Flow B (AOT) host on the Waveshare RP2040-Touch-LCD-1.69
# (RP2040, 1.69" rounded-rectangle 240x280 ST7789V2 SPI panel, CST816S capacitive touch).
#   mkdir build && cd build && cmake .. && make          # → embedded-react-watch.uf2
#
# Flow B needs NO QuickJS: this pulls in the embedded-react engine, runtime + the pico-spi-lcd backend —
# from the LOCAL monorepo checkout when building in-tree, else fetched from GitHub. The app is a package
# written to a slot, separately from the firmware (embedded-react flash).
#
# The pico-sdk is located via the standard shim: set PICO_SDK_PATH, or PICO_SDK_FETCH_FROM_GIT=1 to
# have CMake download it.
cmake_minimum_required(VERSION 3.16)

include(pico_sdk_import.cmake)

include(FetchContent)

# --- embedded-react engine + backend (local in-repo, else fetched) ---------------------------------
# ../../.. = repo root (this project lives at examples/rp2040/rp2040-touch-lcd-1.69/).
set(_er_local "${CMAKE_CURRENT_LIST_DIR}/../../..")
if(EXISTS "${_er_local}/engine/include/er_scene.h")
    set(EMBEDDED_REACT_SRC_DIR "${_er_local}" CACHE INTERNAL "embedded-react source (local monorepo)")
    message(STATUS "embedded-react: using local monorepo source at ${_er_local}")
else()
    FetchContent_Declare(embedded_react
        GIT_REPOSITORY https://github.com/TheMasterCoder007/embedded-react.git
        GIT_TAG        v0.19.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _skip_cmake_
    )
    message(STATUS "embedded-react: fetching engine + backend from GitHub ...")
    FetchContent_MakeAvailable(embedded_react)
    set(EMBEDDED_REACT_SRC_DIR "${embedded_react_SOURCE_DIR}" CACHE INTERNAL "embedded-react source (fetched)")
endif()

project(embedded-react-watch C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

# --- Package slots --------------------------------------------------------------------------------
# Two 512 KB slots above the firmware, in the 2 MB the Pico SDK assumes of this board's flash. Keep these in
# step with packages/embedded-react/flash.mjs. stack_guard.ld fails the link if the firmware reaches them.
set(ER_PACKAGE_SLOT_A_OFFSET 0x100000)
set(ER_PACKAGE_SLOT_B_OFFSET 0x180000)
set(ER_PACKAGE_SLOT_BYTES 0x80000)

# --- embedded-react engine, RAM-tuned for the RP2040 (264 KB SRAM, no FPU) -------------------------
# The 240x280 RGB565 framebuffer is 131.3 KB, so every static pool must fit in what's left. Same
# philosophy as the no-PSRAM CYD example (examples/esp32/esp32-2432s028r): shadows / 3D / gradients /
# keyboard off, node pool sized to the watch-face demo (34 nodes) with headroom, vector pools sized
# for its two <Svg> layers (the 60-segment tick ring is the big one).
set(E "${EMBEDDED_REACT_SRC_DIR}/engine")
add_library(er_engine STATIC
    "${E}/core/native_renderer.c"
    "${E}/core/er_parallel.c"
    "${E}/scene/arc_widget.c"
    "${E}/scene/compositor.c"
    "${E}/scene/damage.c"
    "${E}/scene/hit_test.c"
    "${E}/layout/layout_engine.c"
    "${E}/rendering/arc.c"
    "${E}/rendering/canvas_bindings.c"
    "${E}/rendering/error_screen.c"
    "${E}/rendering/gradient.c"
    "${E}/rendering/image_registry.c"
    "${E}/rendering/image_scaler.c"
    "${E}/rendering/perf_overlay.c"
    "${E}/rendering/perf_stats.c"
    "${E}/rendering/rrect.c"
    "${E}/rendering/scratch_pool.c"
    "${E}/rendering/shadow.c"
    "${E}/rendering/transform.c"
    "${E}/rendering/vector.c"
    "${E}/rendering/vector_cache.c"
    "${E}/rendering/vector_store.c"
    "${E}/text/text_renderer.c"
    "${E}/animation/animation.c"
    "${E}/animation/layout_anim.c"
    "${E}/font/font_bitmap.c"
    "${E}/font/font_blob.c"
    "${E}/font/font_data.c"
    "${E}/font/font_registry.c"
)
target_include_directories(er_engine PUBLIC "${E}/include")
target_include_directories(er_engine PRIVATE
    "${E}/core" "${E}/scene" "${E}/layout" "${E}/rendering" "${E}/text" "${E}/animation" "${E}/font")

# PUBLIC so dependents (the runtime, main) agree on struct layout (er_scene.h).
target_compile_definitions(er_engine PUBLIC
    ERUI_SHADOWS=0
    ERUI_BORDER_AA=1
    ERUI_3D_TRANSFORMS=0
    ERUI_BILINEAR_SCALE=0
    ERUI_GRADIENT=0
    ERUI_ONSCREEN_KEYBOARD=0
    ERUI_TRANSFORMS_TRANSLATE_ONLY=1
    ERUI_FONT_SIZES=7
    ERUI_MAX_NODES=48  # two-page pager (watch face + level page) is 42 nodes; each node is ~1.3 KB of .bss
                       # (plus per-node layout work-arrays), so keep this trimmed to fit beside the framebuffer
    ERUI_MAX_OPACITY_DEPTH=1
    ERUI_SCRATCH_W=8
    ERUI_SCRATCH_H=8
    ERUI_FONT_POOL_BYTES=0
    ERUI_MAX_IMG_ROW_PIXELS=240
    ER_DAMAGE_RECTS_MAX=4      # the engine default (16) pays ~1.2 KB of .bss (6 rect sets) to keep clips
                               # tight on screens full of scattered updaters. A watch face has a handful
                               # of updaters, and this board has ~15 KB of heap left after the
                               # 240x280 framebuffer, so it takes the smaller budget.
    ERUI_IMAGE_REGISTRY_MAX=8  # the watch face registers none; the engine default (128) is sized for
                               # icon-heavy apps and would cost ~10 KB of .bss this board cannot spare
    ERUI_MAX_ANIMATIONS=16     # the watch face runs none; the defaults (64 animations, 32 values, 8 groups)
    ERUI_MAX_ANIM_VALUES=8     # cost ~16 KB of .bss, and the Flow B runtime needs that room. A package that
    ERUI_MAX_ANIM_GROUPS=4     # needs more is rejected at load, saying which
)
# Vector rasterizer pools (internal to vector.c → PRIVATE). The current digital watch face uses NO <Svg>,
# so these are sized to the floor — just enough that the pools exist. Bump them if a demo brings back
# vector art (e.g. an analog dial): the edge pool + its three edge-sized work arrays are the second-biggest
# .bss cost after the node pool, so shrinking them here buys back a lot of headroom beside the framebuffer.
target_compile_definitions(er_engine PRIVATE
    ERUI_VECTOR_MAX_PTS=64
    ERUI_VECTOR_MAX_SUBPATHS=8
    ERUI_VECTOR_MAX_EDGES=256
    ERUI_VECTOR_MAX_ROW=240
    ERUI_MAX_VECTOR_NODES=1
    ERUI_VECTOR_EDGE_CACHE=0     # cached <Svg> edge lists: this face uses no vector nodes, save the RAM
    ER_PERF_OVERLAY=0
)
# The per-node STORAGE caps are PUBLIC, unlike the rasterizer scratch above: er_scene.h documents them as
# the sizes a CALLER marshals into, so anything linking the engine has to see the values this board was
# actually built with rather than the header's defaults.
target_compile_definitions(er_engine PUBLIC
    ERUI_VECTOR_TAPE_MAX=64
    ERUI_VECTOR_PAINTS_MAX=8
    ERUI_VECTOR_GRADS_MAX=1
)

# --- Flow B runtime --------------------------------------------------------------------------------
# It checks a package and runs it, with no QuickJS and no heap.
set(R "${EMBEDDED_REACT_SRC_DIR}/runtime")
add_library(er_runtime STATIC
    "${R}/er_container.c"
    "${R}/er_assets.c"
    "${R}/erb_props.c"
    "${R}/erb_verify.c"
    "${R}/erb_vm.c"
    "${R}/erb_runtime.c"
    "${R}/erb_package_slots.c"
)
target_include_directories(er_runtime PUBLIC "${R}")
target_link_libraries(er_runtime PUBLIC er_engine)
# How large an app this firmware accepts: most of these size the verifier's fixed scratch, beside the framebuffer.
# PUBLIC, since erb_internal.h sizes structs by them.
target_compile_definitions(er_runtime PUBLIC
    ERB_MAX_FUNCTIONS=64
    ERB_MAX_FUNCTION_WORDS=4096
    ERB_MAX_LOOPS=16
    ERB_REPORT_PLACES=16
)
# The VM's dispatch loop stays in flash: at about 12 KB it doesn't fit in SRAM beside the framebuffer.

# --- firmware --------------------------------------------------------------------------------------
add_executable(embedded-react-watch
    main.c
    board.c
    pedometer.c
    "${EMBEDDED_REACT_SRC_DIR}/backends/pico-spi-lcd/renderer_backend.c"
)
target_include_directories(embedded-react-watch PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${EMBEDDED_REACT_SRC_DIR}/backends/pico-spi-lcd"
)
target_link_libraries(embedded-react-watch
    er_engine
    er_runtime
    pico_stdlib
    hardware_flash
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_sync
    hardware_watchdog
)

# Core 0's stack is 8 KB: all of SCRATCH_Y here, plus SCRATCH_X, which main.c claims as its lower half
# (core 1 never runs). The watch face peaks at ~3.1 KB; the rest is headroom for deeper apps. main.c also puts a 2 KB
# MPU guard between the stack and the heap, so an overflow faults instead of corrupting the framebuffer;
# stack_guard.ld fails the link if .data/.bss ever grow into that guard.
set(ER_STACK_GUARD_BYTES 2048)
math(EXPR ER_FRAMEBUFFER_BYTES "240 * 280 * 2") # malloc'd at boot, so the link checks it fits: see stack_guard.ld
target_compile_definitions(embedded-react-watch PRIVATE
    PICO_STACK_SIZE=0x1000
    STACK_GUARD_BYTES=${ER_STACK_GUARD_BYTES}U
    ER_PACKAGE_SLOT_A_OFFSET=${ER_PACKAGE_SLOT_A_OFFSET}U
    ER_PACKAGE_SLOT_B_OFFSET=${ER_PACKAGE_SLOT_B_OFFSET}U
    ER_PACKAGE_SLOT_BYTES=${ER_PACKAGE_SLOT_BYTES}U
)
target_link_options(embedded-react-watch PRIVATE
    "LINKER:--defsym=__stack_guard_bytes=${ER_STACK_GUARD_BYTES}"
    "LINKER:--defsym=__framebuffer_bytes=${ER_FRAMEBUFFER_BYTES}"
    "LINKER:--defsym=__package_slots_offset=${ER_PACKAGE_SLOT_A_OFFSET}"
    "${CMAKE_CURRENT_LIST_DIR}/stack_guard.ld"
)

# Bring-up debug aids (see main.c): -DER_BOARD_DEBUG=1 waits for USB + heartbeat-logs; =2 also holds
# a color-bar test pattern at boot.
if(DEFINED ER_BOARD_DEBUG)
    target_compile_definitions(embedded-react-watch PRIVATE ER_BOARD_DEBUG=${ER_BOARD_DEBUG})
endif()

# A forced failure, to check the error screen on hardware (see main.c): 1 = a hard fault, 2 = a hang.
if(DEFINED ER_BOARD_CRASH_TEST)
    target_compile_definitions(embedded-react-watch PRIVATE ER_BOARD_CRASH_TEST=${ER_BOARD_CRASH_TEST})
endif()

# Log over USB CDC (the board's only exposed port); UART pins aren't broken out usefully here.
pico_enable_stdio_usb(embedded-react-watch 1)
pico_enable_stdio_uart(embedded-react-watch 0)

pico_add_extra_outputs(embedded-react-watch)  # → .uf2 for BOOTSEL drag-and-drop / picotool
