# embedded-react engine as an ESP-IDF component, compiled from EMBEDDED_REACT_SRC_DIR (set by the
# top-level CMakeLists — the local monorepo checkout in-tree, else the FetchContent'd GitHub copy).
# The engine is platform-neutral C99. Source list mirrors engine/CMakeLists.txt; keep the ERUI_*
# flags in sync — they affect public struct layout (er_scene.h), so the bridge + main must see
# identical values (hence PUBLIC).
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
    ERUI_RENDER_WORKERS=2
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
    # Edge cache (vector_cache.c): cached rasterizer geometry so a static <Svg> repainted under a moving
    # sibling's damage skips re-tessellation. The pool also lives in PSRAM (linker_psram.lf) — replay
    # reads edges through the PSRAM cache, still far cheaper than rebuilding them. ~85 KB per node.
    ERUI_VECTOR_EDGE_CACHE=1
    ERUI_VECTOR_CACHE_NODES=2
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

# Engine frame instrumentation (perf_stats.c + the compositor's layout/raster marks). Left to itself
# ER_PERF_STATS would follow ER_PERF_OVERLAY (see er_perf.h); key it off ER_PERF_DETAIL instead so the
# everyday overlay carries no per-frame instrumentation cost and shows no engine diagnostic lines —
# just FPS / CPU / PSRAM / IRAM. Build with `idf.py -DER_PERF_DETAIL=1` when debugging a spike;
# main/CMakeLists.txt passes the same switch to main.c, which appends the engine's overlay lines
# (FRM/PK split, PKDRT, VEC/IMG slots) only then.
if(NOT DEFINED ER_PERF_DETAIL)
    set(ER_PERF_DETAIL 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ER_PERF_STATS=${ER_PERF_DETAIL})

# The engine is platform-neutral C99 developed against desktop GCC; the xtensa GCC 14 toolchain is
# stricter and IDF builds with -Werror. Don't let IDF's policy fail the build on the engine's
# (warning-level) diagnostics — keep them as warnings rather than escalate engine code here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
