cmake_minimum_required(VERSION 3.14)
project(embedded-react-engine C)
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Feature flags ("Compile-Time Feature Flags" — see /PLAN.md appendix)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Defaults are tuned for resource-constrained MCUs (minimum flash / RAM).
# You build should override these as needed — see examples/linux/CMakeLists.txt
# for a reference configuration that enables everything.
# ---------------------------------------------------------------------------
option(ERUI_SHADOWS "Enable box shadow rasterizer (significant flash + scratch RAM)" OFF)
option(ERUI_BORDER_AA "Anti-aliased corner edges on rounded rects" ON)
option(ERUI_3D_TRANSFORMS "Enable rotateX/rotateY/perspective (requires ERUI_TRANSFORMS=FULL)" OFF)
option(ERUI_BILINEAR_SCALE "Bilinear image scaling (vs nearest-neighbor; ~1 KB extra)" OFF)
option(ERUI_GRADIENT "Enable gradient rasterizer (~1 KB extra)" OFF)
option(ERUI_GRADIENT_RADIAL "Enable radial gradients (requires ERUI_GRADIENT)" OFF)

# TRANSLATE_ONLY: pure-integer pixel shifts, no scratch buffers needed for transforms.
# FULL: full 2D affine (scale/rotate/shear) via scratch-buffer back-projection.
# Set to FULL on desktop or any target that can afford the scratch RAM.
set(ERUI_TRANSFORMS "TRANSLATE_ONLY" CACHE STRING "FULL or TRANSLATE_ONLY")
set(ERUI_FONT_SIZES "7" CACHE STRING "Number of pre-rasterized font sizes")
set(ERUI_MAX_NODES "512" CACHE STRING "Scene graph node pool size")
set(ERUI_MAX_OPACITY_DEPTH "4" CACHE STRING "Maximum nested offscreen-composite layers")
set(ERUI_SCRATCH_W "240" CACHE STRING "Scratch buffer width in pixels (max compositable node width)")
set(ERUI_SCRATCH_H "240" CACHE STRING "Scratch buffer height in pixels (max compositable node height)")
set(ERUI_FONT_POOL_BYTES "0" CACHE STRING "Static pool for runtime-loaded fonts (bytes); 0 disables er_font_load")
set(ERUI_MAX_IMG_ROW_PIXELS "800" CACHE STRING "Static row buffer width for nearest-neighbor image scaling")
set(ERUI_MAX_ANIM_VALUES "16" CACHE STRING "Pool size for standalone Animated.Values (useNativeDriver)")
set(ERUI_MAX_VALUE_BINDINGS "4" CACHE STRING "Max node-property bindings per Animated.Value")

# ---------------------------------------------------------------------------
# Sources — grouped by subsystem to match the engine/ folder layout
# ---------------------------------------------------------------------------
set(EMBEDDED_REACT_SOURCES
        # core: backend glue, frame tick
        core/native_renderer.c

        # scene: node pool, tree, render pass, hit-testing
        scene/compositor.c
        scene/hit_test.c

        # layout: Yoga 7-pass flexbox
        layout/layout_engine.c

        # rendering: primitives (rounded rects, shadows, transforms, image, gradient, canvas)
        rendering/canvas_bindings.c
        rendering/gradient.c
        rendering/image_registry.c
        rendering/image_scaler.c
        rendering/perf_overlay.c
        rendering/rrect.c
        rendering/scratch_pool.c
        rendering/shadow.c
        rendering/transform.c
        rendering/vector.c

        # text: UTF-8 + glyph blits
        text/text_renderer.c

        # animation: Animated.Value engine + layout transitions
        animation/animation.c
        animation/layout_anim.c

        # resources: fonts (built-in + runtime loader)
        font/font_bitmap.c
        font/font_blob.c
        font/font_data.c
        font/font_registry.c
)

add_library(embedded-react STATIC ${EMBEDDED_REACT_SOURCES})

target_include_directories(embedded-react
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/scene
        ${CMAKE_CURRENT_SOURCE_DIR}/layout
        ${CMAKE_CURRENT_SOURCE_DIR}/rendering
        ${CMAKE_CURRENT_SOURCE_DIR}/text
        ${CMAKE_CURRENT_SOURCE_DIR}/animation
        ${CMAKE_CURRENT_SOURCE_DIR}/font
        ${CMAKE_CURRENT_SOURCE_DIR}/platform
)

target_compile_definitions(embedded-react PUBLIC
        ERUI_SHADOWS=$<BOOL:${ERUI_SHADOWS}>
        ERUI_BORDER_AA=$<BOOL:${ERUI_BORDER_AA}>
        ERUI_3D_TRANSFORMS=$<BOOL:${ERUI_3D_TRANSFORMS}>
        ERUI_BILINEAR_SCALE=$<BOOL:${ERUI_BILINEAR_SCALE}>
        ERUI_GRADIENT=$<BOOL:${ERUI_GRADIENT}>
        ERUI_GRADIENT_RADIAL=$<BOOL:${ERUI_GRADIENT_RADIAL}>
        ERUI_TRANSFORMS_${ERUI_TRANSFORMS}=1
        ERUI_FONT_SIZES=${ERUI_FONT_SIZES}
        ERUI_MAX_NODES=${ERUI_MAX_NODES}
        ERUI_MAX_OPACITY_DEPTH=${ERUI_MAX_OPACITY_DEPTH}
        ERUI_SCRATCH_W=${ERUI_SCRATCH_W}
        ERUI_SCRATCH_H=${ERUI_SCRATCH_H}
)

target_compile_definitions(embedded-react PRIVATE
        ERUI_FONT_POOL_BYTES=${ERUI_FONT_POOL_BYTES}
        ERUI_MAX_IMG_ROW_PIXELS=${ERUI_MAX_IMG_ROW_PIXELS}
        ERUI_MAX_ANIM_VALUES=${ERUI_MAX_ANIM_VALUES}
        ERUI_MAX_VALUE_BINDINGS=${ERUI_MAX_VALUE_BINDINGS}
)

target_link_libraries(embedded-react PUBLIC m)

# ---------------------------------------------------------------------------
# Tests (host-side, no MCU toolchain required)
# ---------------------------------------------------------------------------
if (BUILD_TESTING AND CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    add_subdirectory(tests/animation)
    add_subdirectory(tests/input)
    add_subdirectory(tests/layout)
    add_subdirectory(tests/rendering)
    add_subdirectory(tests/resources)
    add_subdirectory(tests/scene)
    add_subdirectory(tests/scroll)
    add_subdirectory(tests/text)
endif ()
