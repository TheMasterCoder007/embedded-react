# embedded-react engine as an ESP-IDF component for the no-PSRAM CYD (ESP32-WROOM-32).
#
# Unlike the ESP32-S3 example, there is NO linker_psram.lf and NO PSRAM: every static pool must fit
# internal DRAM alongside the 150 KB RGB565 framebuffer. So the RAM-driving flags are turned DOWN:
#   - ERUI_MAX_NODES sizes the node pool (sizeof(ERNode) is several hundred bytes — the dominant cost).
#     96 covers the music-player demo built with ER_AOT_LIST_CAP=8 (~53 nodes). Raise only if needed.
#   - The opacity/transform SCRATCH pool is DEPTH x W x H x 4 bytes; kept tiny (8x8x1). A node with
#     opacity < 255 that doesn't fit the scratch simply renders OPAQUE (graceful — no crash), so the
#     demo's art-dim effect is skipped here but everything else works.
#   - Shadows / gradients / 3D transforms / bilinear scaling are off (code + RAM).
set(E "${EMBEDDED_REACT_SRC_DIR}/engine")

idf_component_register(
    SRCS
        "${E}/core/native_renderer.c"
        "${E}/scene/compositor.c"
        "${E}/scene/hit_test.c"
        "${E}/layout/layout_engine.c"
        "${E}/rendering/canvas_bindings.c"
        "${E}/rendering/gradient.c"
        "${E}/rendering/image_registry.c"
        "${E}/rendering/image_scaler.c"
        "${E}/rendering/perf_overlay.c"
        "${E}/rendering/rrect.c"
        "${E}/rendering/scratch_pool.c"
        "${E}/rendering/shadow.c"
        "${E}/rendering/transform.c"
        "${E}/rendering/vector.c"
        "${E}/text/text_renderer.c"
        "${E}/animation/animation.c"
        "${E}/animation/layout_anim.c"
        "${E}/font/font_bitmap.c"
        "${E}/font/font_blob.c"
        "${E}/font/font_data.c"
        "${E}/font/font_registry.c"
    INCLUDE_DIRS
        "${E}/include"
    PRIV_INCLUDE_DIRS
        "${E}/core"
        "${E}/scene"
        "${E}/layout"
        "${E}/rendering"
        "${E}/text"
        "${E}/animation"
        "${E}/font"
)

# RAM-tuned feature flags. PUBLIC so dependents (main) agree on struct layout (er_scene.h).
# sizeof(ERNode) is ~1.3 KB, so ERUI_MAX_NODES dominates .bss (96 → 124 KB). 64 covers the demo's ~54
# nodes (53 + the screen root) with margin. Raise only if a bigger app needs it.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ERUI_SHADOWS=0
    ERUI_BORDER_AA=1
    ERUI_3D_TRANSFORMS=0
    ERUI_BILINEAR_SCALE=0
    ERUI_GRADIENT=0
    ERUI_GRADIENT_RADIAL=0
    ERUI_ONSCREEN_KEYBOARD=1
    ERUI_TRANSFORMS_TRANSLATE_ONLY=1
    ERUI_FONT_SIZES=7
    ERUI_MAX_NODES=64
    ERUI_MAX_OPACITY_DEPTH=1
    ERUI_SCRATCH_W=8
    ERUI_SCRATCH_H=8
    ERUI_FONT_POOL_BYTES=0
    ERUI_MAX_IMG_ROW_PIXELS=240
)

# The SVG/vector rasterizer's static pools are ~100 KB by default — far too big for internal RAM. Sized
# for the thermostat-aot dial (ONE <Svg>: a 270° track arc + value arc + handle circle). The op-tape is
# ~37 floats / 3 paints; MAX_ROW MUST cover the dial node width (240 px) or the coverage row clips. Banded
# RGB565 freed ~56 KB vs the old RGB332 fb, leaving headroom. (Internal to vector.c, so PRIVATE.)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ERUI_VECTOR_MAX_PTS=512      # flattened vertices per shape (a 270° arc + round caps)
    ERUI_VECTOR_MAX_SUBPATHS=8
    ERUI_VECTOR_MAX_EDGES=600   # stroke outline edges per pass — both round caps need full tessellation
    ERUI_VECTOR_MAX_ROW=200      # coverage row buffer in px — must be >= the dial node width (176)
    ERUI_MAX_VECTOR_NODES=1      # the dial is the only vector node
    ERUI_VECTOR_TAPE_MAX=64      # op-tape floats (dial ~37)
    ERUI_VECTOR_PAINTS_MAX=8     # paints (dial = 3)
)

# Perf overlay off (saves code/RAM); the host doesn't gather metrics either.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_OVERLAY=0)

# Engine is platform-neutral C99 vs stricter xtensa GCC + IDF -Werror — keep its diagnostics warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
