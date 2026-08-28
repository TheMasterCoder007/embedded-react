# embedded-react engine as an ESP-IDF component for the no-PSRAM CYD (ESP32-WROOM-32).
#
# Unlike the ESP32-S3 example, there is NO linker_psram.lf and NO PSRAM: every static pool must fit
# internal DRAM alongside the 150 KB RGB565 framebuffer. So the RAM-driving flags are turned DOWN:
#   - ERUI_MAX_NODES sizes the node pool (sizeof(ERNode) is several hundred bytes — the dominant cost).
#     Must cover the thermostat solo dial (39). It is the tightest budget on any supported board.
#   - The opacity/transform SCRATCH pool is DEPTH x W x H x 4 bytes; kept tiny (8x8x1). A node with
#     opacity < 255 that doesn't fit the scratch simply renders OPAQUE (graceful — no crash), so an
#     app's art-dim effect is skipped here but everything else works.
#   - Shadows / 3D transforms are off (code + RAM). GRADIENTS are ON (linear + conic)
#   - Bilinear <Image> scaling is ON (it reuses the existing scaler row buffer — no extra RAM — so icons don't alias).
set(E "${EMBEDDED_REACT_SRC_DIR}/engine")

idf_component_register(
    SRCS
        "${E}/core/native_renderer.c"
        "${E}/core/er_parallel.c"
        "${E}/scene/arc_widget.c"
        "${E}/scene/compositor.c"
        "${E}/scene/damage.c"
        "${E}/scene/hit_test.c"
        "${E}/layout/layout_engine.c"
        "${E}/rendering/arc.c"
        "${E}/rendering/canvas_bindings.c"
        "${E}/rendering/gradient.c"
        "${E}/rendering/image_registry.c"
        "${E}/rendering/image_scaler.c"
        "${E}/rendering/perf_overlay.c"
        "${E}/rendering/perf_stats.c"
        "${E}/rendering/rrect.c"
        "${E}/rendering/scratch_pool.c"
        "${E}/rendering/shadow.c"
        "${E}/rendering/transform.c"
    ERUI_MAX_VECTOR_NODES=32   # storage lives in PSRAM (see linker_psram.lf); raise as needed for many <Svg> nodes
    ERUI_VECTOR_EDGE_CACHE=1
    ERUI_VECTOR_CACHE_NODES=2
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

# RAM-tuned feature flags. PUBLIC so dependents (main) agree on struct layout (er_scene.h)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ERUI_SHADOWS=0
    ERUI_BORDER_AA=1
    ERUI_3D_TRANSFORMS=0
    ERUI_BILINEAR_SCALE=1
    ERUI_GRADIENT=1
    ERUI_GRADIENT_RADIAL=0
    ERUI_GRADIENT_CONIC=1
    ERUI_ONSCREEN_KEYBOARD=1
    ERUI_TRANSFORMS_TRANSLATE_ONLY=1
    ERUI_FONT_SIZES=7
    ERUI_MAX_NODES=44
    ERUI_MAX_OPACITY_DEPTH=1
    ERUI_SCRATCH_W=8
    ERUI_SCRATCH_H=8
    ERUI_FONT_POOL_BYTES=0
    ERUI_MAX_IMG_ROW_PIXELS=240
    ERUI_IMAGE_REGISTRY_MAX=16  # the engine default (128) is sized for icon-heavy apps at ~80 B/slot;
                                # ~10 KB of .bss does not fit beside the 150 KB framebuffer here
    ER_DAMAGE_RECTS_MAX=4       # the engine default (16) buys tight clips on screens full of scattered
                                # updaters; this one is a single dial, so it would spend ~1.2 KB of .bss
                                # (6 rect sets) on rects it can never fill. DRAM here is 94% full.
)

# The SVG/vector rasterizer's static pools
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ERUI_VECTOR_MAX_PTS=512      # flattened vertices per shape (a 270° wide-stroke arc bakes to cubics → ~200+ pts)
    ERUI_VECTOR_MAX_SUBPATHS=8
    ERUI_VECTOR_MAX_EDGES=1536   # stroke-outline edges for the largest single shape. A wide-stroke 240° arc
                                 # tessellates to >600, and 600 truncated on HW (ERUI_VECTOR_MAX_EDGES
                                 # exhausted warning), so keep this well clear of the arc's edge count.
    ERUI_VECTOR_MAX_ROW=224      # coverage row buffer in px. MUST be >= the widest vector node.
    ERUI_MAX_VECTOR_NODES=2      # the dial is a native <Dial> now and uses NO vector node; this whole
                                 # pool is spare headroom for an app that adds an <Svg>
    ERUI_VECTOR_TAPE_MAX=256     # op-tape floats per node (the dial is ~45)
    ERUI_VECTOR_PAINTS_MAX=16    # paints per node (the dial uses 4)
    ERUI_VECTOR_GRADS_MAX=2      # gradient entries per node (the dial uses none; default 8 → trim RAM)
    ERUI_VECTOR_EDGE_CACHE=0     # cached edge lists for static <Svg> nodes: ~85 KB/node of .bss this
                                 # board does not have; the thermostat's dial is a native <Dial> anyway
)

# Perf overlay off (saves code/RAM); the host doesn't gather metrics either.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_OVERLAY=0)

# Engine is platform-neutral C99 vs stricter xtensa GCC + IDF -Werror — keep its diagnostics warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
