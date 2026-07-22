# embedded-react engine as an ESP-IDF component, compiled from EMBEDDED_REACT_SRC_DIR (set by the
# top-level CMakeLists — the local monorepo checkout in-tree, else the FetchContent'd GitHub copy).
# The engine is platform-neutral C99. Source list mirrors engine/CMakeLists.txt; keep the ERUI_*
# flags in sync — they affect public struct layout (er_scene.h), so the bridge + main must see
# identical values (hence PUBLIC).
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
        "${E}/rendering/vector_store.c"
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
    LDFRAGMENTS "linker_psram.lf"
)

# Feature flags — PUBLIC so dependents (bridge, main) agree on struct layout.
#
# ERUI_SCRATCH_W/BAND_H size the opacity-compositing strip pool (kept in internal DRAM, see
# linker_psram.lf); ERUI_XFORM_W/H cap the largest node that can be rotated or scaled. See
# engine/README.md "Scratch buffers" for what each controls.
# ERUI_TRANSFORMS=FULL enables the 2D affine path (scale/rotate) the starter's
# Animated.timing(scale) logo pulse needs; under TRANSLATE_ONLY that pulse silently does
# nothing.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ERUI_SHADOWS=0
    ERUI_BORDER_AA=1
    ERUI_3D_TRANSFORMS=0
    ERUI_BILINEAR_SCALE=1
    ERUI_GRADIENT=1
    ERUI_GRADIENT_RADIAL=0
    ERUI_GRADIENT_CONIC=1
    ERUI_TRANSFORMS_FULL=1
    ERUI_FONT_SIZES=7
    ERUI_MAX_NODES=512
    ERUI_MAX_OPACITY_DEPTH=2
    ERUI_SCRATCH_W=800
    ERUI_SCRATCH_H=240
    ERUI_SCRATCH_BAND_H=10
    ERUI_XFORM_W=240
    ERUI_XFORM_H=240
    ERUI_FADE_CACHE_W=800
    ERUI_FADE_CACHE_H=480
    ERUI_FONT_POOL_BYTES=0
    ERUI_MAX_IMG_ROW_PIXELS=800
)

# Vector (SVG) pools — PRIVATE (internal to vector.c / vector_store.c; no public struct impact). The
# per-node op-tape STORAGE pool (vector_store.o) is mapped to PSRAM by linker_psram.lf, so
# ERUI_MAX_VECTOR_NODES can be raised well past the internal-RAM-bound default of 8 — handy for icon- or
# vector-heavy screens. The hot rasterize scratch (vector.o) stays in internal DRAM, so its caps
# (MAX_PTS / MAX_EDGES / MAX_ROW) keep the engine defaults.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ERUI_MAX_VECTOR_NODES=32   # storage lives in PSRAM (see linker_psram.lf); raise as needed for many <Svg> nodes
    # ER_PROF=1                # uncomment for on-device compositor phase profiling (prints ERPROF lines)
)

# Performance overlay (FPS/CPU/memory, bottom-right). The actual drawing lives in this component's
# perf_overlay.c, which is a no-op stub unless ER_PERF_OVERLAY=1 reaches it — so it must be set HERE,
# not only on the main component (which gathers the host-side metrics). Keep this in sync with
# main/CMakeLists.txt; set both to 0 to compile the overlay out entirely.
if(NOT DEFINED ER_PERF_OVERLAY)
    set(ER_PERF_OVERLAY 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_OVERLAY=${ER_PERF_OVERLAY})

# The engine is platform-neutral C99 developed against desktop GCC; the xtensa GCC 14 toolchain is
# stricter and IDF builds with -Werror. Don't let IDF's policy fail the build on the engine's
# (warning-level) diagnostics — keep them as warnings rather than escalate engine code here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
