cmake_minimum_required(VERSION 3.16)
project(embedded-react C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Feature flags ("Compile-Time Feature Flags")
# ---------------------------------------------------------------------------
option(ERUI_SHADOWS "Enable box shadow rasterizer" OFF)
option(ERUI_BORDER_AA "Anti-aliased border radius edges" ON)
option(ERUI_3D_TRANSFORMS "Enable rotateX/rotateY/perspective" OFF)
option(ERUI_BILINEAR_SCALE "Bilinear image scaling (vs nearest-neighbor)" OFF)
option(ERUI_GRADIENT "Enable gradient rasterizer" ON)
option(ERUI_GRADIENT_RADIAL "Enable radial gradients (requires ERUI_GRADIENT)" ON)
option(ERUI_JS_BRIDGE "Include QuickJS bridge adapter" ON)

set(ERUI_TRANSFORMS "FULL" CACHE STRING "FULL or TRANSLATE_ONLY")
set(ERUI_FONT_SIZES "7" CACHE STRING "Number of pre-rasterized font sizes")
set(ERUI_MAX_NODES "512" CACHE STRING "Scene graph node pool size")
set(ERUI_MAX_OPACITY_DEPTH "4" CACHE STRING "Maximum nested offscreen-composite layers")
set(ERUI_FONT_POOL_BYTES "0" CACHE STRING "Static pool for runtime-loaded fonts (bytes); 0 disables er_font_load")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(EMBEDDED_REACT_SOURCES
        src/native_renderer.c
        src/layout_engine.c
        src/text_renderer.c
        src/font_registry.c
        src/font_blob.c
        src/font_data.c
        src/font_bitmap.c
        src/image_scaler.c
        src/rrect.c
        src/shadow.c
        src/transform.c
        src/animation.c
        src/compositor.c
        src/hit_test.c
        src/canvas_bindings.c
)

if (ERUI_JS_BRIDGE)
    list(APPEND EMBEDDED_REACT_SOURCES src/bridges/quickjs/native_ui_bridge.c)
endif ()

add_library(embedded-react STATIC ${EMBEDDED_REACT_SOURCES})

target_include_directories(embedded-react
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(embedded-react PUBLIC
        ERUI_SHADOWS=$<BOOL:${ERUI_SHADOWS}>
        ERUI_BORDER_AA=$<BOOL:${ERUI_BORDER_AA}>
        ERUI_3D_TRANSFORMS=$<BOOL:${ERUI_3D_TRANSFORMS}>
        ERUI_BILINEAR_SCALE=$<BOOL:${ERUI_BILINEAR_SCALE}>
        ERUI_GRADIENT=$<BOOL:${ERUI_GRADIENT}>
        ERUI_GRADIENT_RADIAL=$<BOOL:${ERUI_GRADIENT_RADIAL}>
        ERUI_JS_BRIDGE=$<BOOL:${ERUI_JS_BRIDGE}>
        ERUI_TRANSFORMS_${ERUI_TRANSFORMS}=1
        ERUI_FONT_SIZES=${ERUI_FONT_SIZES}
        ERUI_MAX_NODES=${ERUI_MAX_NODES}
        ERUI_MAX_OPACITY_DEPTH=${ERUI_MAX_OPACITY_DEPTH}
)

target_compile_definitions(embedded-react PRIVATE
        ERUI_FONT_POOL_BYTES=${ERUI_FONT_POOL_BYTES}
)

target_link_libraries(embedded-react PUBLIC m)

# ---------------------------------------------------------------------------
# Tests (host-side, no MCU toolchain required)
# ---------------------------------------------------------------------------
if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests/layout)
    add_subdirectory(tests/text)
endif ()
