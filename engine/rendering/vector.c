/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Vector path rasterizer — anti-aliased fills and strokes for the ER_NODE_VECTOR (Svg) node.
 *
 * Pipeline (clean-room, modelled on the standard scanline-coverage approach used by nanosvg/FreeType):
 *   1. Flatten the op-tape's path ops (lines, quadratic/cubic beziers, circular arcs) into polylines,
 *      grouped into subpaths.
 *   2. FILL: turn every subpath into closed edges and rasterize with the paint's winding rule.
 *   3. STROKE: expand each subpath polyline into outline geometry (segment quads + caps + joins) and
 *      rasterize that as a nonzero fill in the stroke color.
 *   4. Rasterize = per pixel row, take SUBSAMPLES sub-scanlines, find edge crossings, walk them by
 *      winding to get covered spans, accumulate fractional horizontal coverage, then blend the row's
 *      AA spans through er_blit_fill (which composites — incl. into the RGB565 framebuffer).
 *
 * All working buffers are static and bounded (no allocation); the engine is single-threaded so reuse
 * across shapes is safe. Coordinates from the tape are node-local; the node's screen origin (px,py) is
 * added during flattening, and all painting is clipped to the node box.
 */

#include "vector.h"

#include "vector_cache.h" /* ERVecEdge + the cache entry layout (record/replay share it with vector_cache.c) */

#include "arc.h"               /* er_arc_fill_sector — the shared analytic core (ERUI_VECTOR_ANALYTIC_ARC) */
#include "renderer_internal.h" /* er_blit_fill */

#include <math.h>
#include <stdbool.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Tunables
 ---------------------------------------------------------------------------------------------------------------------*/

/* These back STATIC working buffers that the rasterizer touches per-pixel/per-scanline, so on a
 * PSRAM-bss target (ESP32) they must stay in INTERNAL RAM (see the engine component's linker fragment)
 * — sized to fit there. Raise them (and the internal-RAM budget) for very complex paths. */
#ifndef ERUI_VECTOR_MAX_PTS
#define ERUI_VECTOR_MAX_PTS 2048 /**< Flattened polyline vertices per shape. */
#endif
#ifndef ERUI_VECTOR_MAX_SUBPATHS
#define ERUI_VECTOR_MAX_SUBPATHS 256
#endif
#ifndef ERUI_VECTOR_MAX_EDGES
#define ERUI_VECTOR_MAX_EDGES 2048 /**< Edges submitted to one rasterize pass. */
#endif
#ifndef ERUI_VECTOR_MAX_ROW
#define ERUI_VECTOR_MAX_ROW 1024 /**< Max node width in px (coverage row buffer). */
#endif
#ifndef ERUI_VECTOR_SORT_BUCKETS
#define ERUI_VECTOR_SORT_BUCKETS                                                                                       \
    256 /**< Buckets in the edge counting sort. One per clip row until the clip is taller                              \
           than this, then one bucket covers 2/4/... rows — which only activates a few edges                         \
           early, so this is a memory knob, not a correctness one. */
#endif

/* The per-node STORAGE pools (ERUI_MAX_VECTOR_NODES / ERUI_VECTOR_TAPE_MAX / ERUI_VECTOR_PAINTS_MAX) live
 * with that pool in vector_store.c so it can be placed in PSRAM independently of this file's hot scratch. */

#define VEC_SUBSAMPLES 4   /**< Vertical AA sub-scanlines per pixel row. */
#define VEC_FLAT_TOL 0.18f /**< Bezier flatness tolerance (px²). Coarser = fewer edges (AA hides facets). */
#define VEC_ARC_TOL                                                                                                    \
    0.10f /**< Arc chord-error tolerance (px). Geometry build is ~1ms, so keep this fine:                              \
             the rasterize cost is clip-area bound, not edge-count bound, and a coarse                                 \
             value visibly facets small curves (0.25f measures ~13% faster on a stroked                                \
             ring and shifts its edge by a visible quarter pixel — not worth it). */
#define VEC_JOIN_TOL 0.05f  /**< How far (px) a merged stroke corner may stray from the join it replaces. */
#define VEC_CLOSE_EPS 0.05f /**< Under this gap, a CLOSE that misses its start point counts as landing on it. */
#define VEC_PI 3.14159265358979323846f
#define VEC_RAD2DEG 57.29577951308232f

/* Route a shape that IS just a circular arc at the shared analytic core (rendering/arc.c) instead of
 * flattening + stroking it — so an <Arc> or <Circle> in an <Svg> rasterizes through exactly the same
 * code (and anti-aliases identically to) a native ER_NODE_ARC widget, at a fraction of the cost. Only
 * shapes that map EXACTLY are routed (see arc_run_match / arc_paint_ok); everything else keeps the
 * general path, so this can never change a shape it does not fully describe. Set to 0 to restore the
 * pre-#87 all-tessellated behaviour. */
#ifndef ERUI_VECTOR_ANALYTIC_ARC
#define ERUI_VECTOR_ANALYTIC_ARC 1
#endif

/* ERUI_VECTOR_DIAGNOSTICS + ERUI_VEC_WARN_ONCE live in vector.h (shared with vector_store.c). */

/*----------------------------------------------------------------------------------------------------------------------
 - Working state (static, reused per render)
 ---------------------------------------------------------------------------------------------------------------------*/

/* One rasterizer edge: ERVecEdge, defined in vector_cache.h — the edge cache (vector_cache.c) stores
 * arrays of them, and replay hands them straight back to rasterize(). */

typedef struct
{
    int start; /**< Index into s_px/s_py of the first vertex. */
    int count; /**< Vertex count. */
    int closed;
} VecSub;

static float s_px[ERUI_VECTOR_MAX_PTS];
static float s_py[ERUI_VECTOR_MAX_PTS];
static int s_npts;
static VecSub s_sub[ERUI_VECTOR_MAX_SUBPATHS];
static int s_nsub;

static ERVecEdge s_edges[ERUI_VECTOR_MAX_EDGES];
static int s_nedges;
static bool s_edge_trunc;              /**< The current build dropped an edge on the pool cap (recording aborts). */
static float s_edge_ytop, s_edge_ybot; /**< Clip rows for the current pass; edge_add drops edges outside them. */

/* Edge ordering for the active-edge table: a counting sort writes indices into s_order, and s_bkt ends
 * up holding each bucket's END offset (see rasterize). Indices, not edges, so the sort moves 2 bytes
 * per edge instead of 20 — this is hot per-frame internal RAM. */
#if ERUI_VECTOR_MAX_EDGES <= 65535
typedef uint16_t VecIdx;
#else
typedef uint32_t VecIdx;
#endif
static VecIdx s_order[ERUI_VECTOR_MAX_EDGES];
static VecIdx s_bkt[ERUI_VECTOR_SORT_BUCKETS];

static float s_cover[ERUI_VECTOR_MAX_ROW]; /**< Per-pixel coverage accumulator for the current row. */
static int s_row_lo, s_row_hi;             /**< Touched x-range (clip-local, [lo,hi)) in s_cover for the current row. */

/* Crossing list reused per sub-scanline. */
static float s_cross_x[ERUI_VECTOR_MAX_EDGES];
static int s_cross_d[ERUI_VECTOR_MAX_EDGES];

#if ERUI_VECTOR_EDGE_CACHE
/* Edge-cache recording state. Non-NULL while er_vector_render_slot is recording a render into a cache
 * entry (vector_cache.c owns the pool). While recording, edge builds run UNCLIPPED (the cache must
 * hold the node's whole geometry, not just this clip's slice) and the per-shape clip reject is off —
 * a one-render cost paid only for a tape that already proved static (see er_vector_cache_begin). */
static ERVecCache* s_rec;
static bool s_rec_failed; /**< The recording overflowed/truncated; entry will be discarded. */
#define VEC_RECORDING() (s_rec != 0 && !s_rec_failed)
#else
#define VEC_RECORDING() false
#endif

/* The per-node op-tape/paint storage pool (er_vector_store/free/reset/slot_ops/slot_paints + s_slots) lives
 * in vector_store.c — a separate translation unit so a target can place that cold pool in PSRAM (via the
 * engine's linker fragment) while this file's hot per-pixel scratch above stays in fast internal RAM. */

/*----------------------------------------------------------------------------------------------------------------------
 - Geometry building
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Appends a vertex to the current subpath (last entry of s_sub). */
static void pt_add(float x, float y)
{
    if (s_npts >= ERUI_VECTOR_MAX_PTS)
    {
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_MAX_PTS", ERUI_VECTOR_MAX_PTS);
        return;
    }
    if (s_nsub == 0)
        return;
    /* Drop exact duplicates (zero-length segments add nothing and bloat the edge list). */
    VecSub* sp = &s_sub[s_nsub - 1];
    if (sp->count > 0)
    {
        const int last = sp->start + sp->count - 1;
        if (s_px[last] == x && s_py[last] == y)
            return;
    }
    s_px[s_npts] = x;
    s_py[s_npts] = y;
    s_npts++;
    sp->count++;
}

/** @brief Starts a new subpath at (x,y). */
static void sub_begin(float x, float y)
{
    if (s_nsub >= ERUI_VECTOR_MAX_SUBPATHS)
    {
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_MAX_SUBPATHS", ERUI_VECTOR_MAX_SUBPATHS);
        return;
    }
    s_sub[s_nsub].start = s_npts;
    s_sub[s_nsub].count = 0;
    s_sub[s_nsub].closed = 0;
    s_nsub++;
    pt_add(x, y);
}

/** @brief Recursively flattens a cubic bezier into line vertices. */
static void flatten_cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int depth)
{
    if (depth > 12)
    {
        pt_add(x3, y3);
        return;
    }
    const float dx = x3 - x0;
    const float dy = y3 - y0;
    const float d1 = fabsf((x1 - x3) * dy - (y1 - y3) * dx);
    const float d2 = fabsf((x2 - x3) * dy - (y2 - y3) * dx);
    if ((d1 + d2) * (d1 + d2) < VEC_FLAT_TOL * (dx * dx + dy * dy))
    {
        pt_add(x3, y3);
        return;
    }
    const float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    const float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    const float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
    const float xa = (x01 + x12) * 0.5f, ya = (y01 + y12) * 0.5f;
    const float xb = (x12 + x23) * 0.5f, yb = (y12 + y23) * 0.5f;
    const float xm = (xa + xb) * 0.5f, ym = (ya + yb) * 0.5f;
    flatten_cubic(x0, y0, x01, y01, xa, ya, xm, ym, depth + 1);
    flatten_cubic(xm, ym, xb, yb, x23, y23, x3, y3, depth + 1);
}

/** @brief Appends a circular arc, sampled so the chord error stays sub-pixel. */
static void append_arc(float cx, float cy, float r, float a0, float a1, int ccw)
{
    if (r < 0.0f)
        r = -r;
    float da = a1 - a0;
    if (ccw)
    {
        while (da > 0.0f)
            da -= 2.0f * VEC_PI;
    }
    else
    {
        while (da < 0.0f)
            da += 2.0f * VEC_PI;
    }
    /* Step angle so the chord deviation r*(1-cos(step/2)) stays under VEC_ARC_TOL px. */
    float step = (r > 0.5f) ? 2.0f * acosf(1.0f - VEC_ARC_TOL / r) : VEC_PI;
    if (step <= 0.0f || step != step) /* NaN/0 guard */
        step = 0.2f;
    int n = (int)ceilf(fabsf(da) / step);
    if (n < 1)
        n = 1;
    for (int i = 0; i <= n; i++)
    {
        const float t = a0 + da * ((float)i / (float)n);
        pt_add(cx + r * cosf(t), cy + r * sinf(t));
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Edge list + rasterization
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Adds a non-horizontal edge to the rasterizer edge list (normalized so y0 <= y1). */
static void edge_add(float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;

    if ((y0 <= s_edge_ytop && y1 <= s_edge_ytop) || (y0 >= s_edge_ybot && y1 >= s_edge_ybot))
        return;
    if (s_nedges >= ERUI_VECTOR_MAX_EDGES)
    {
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_MAX_EDGES", ERUI_VECTOR_MAX_EDGES);
        s_edge_trunc = true;
        return;
    }
    ERVecEdge* e = &s_edges[s_nedges++];
    if (y0 < y1)
    {
        e->x0 = x0;
        e->y0 = y0;
        e->x1 = x1;
        e->y1 = y1;
        e->dir = 1;
    }
    else
    {
        e->x0 = x1;
        e->y0 = y1;
        e->x1 = x0;
        e->y1 = y0;
        e->dir = -1;
    }
}

/** @brief Adds coverage for a covered horizontal span [xs,xe] (screen px) at the given weight. */
static void cover_span(float xs, float xe, float weight, int clipx0, int width)
{
    /* Reject non-finite spans: NaN/inf coordinates (from a malformed path) survive the clamps below and
     * turn (int)floorf(...) into a wild index that writes s_cover[] out of bounds. Defense in depth. */
    if (!isfinite(xs) || !isfinite(xe))
        return;
    xs -= (float)clipx0;
    xe -= (float)clipx0;
    if (xs < 0.0f)
        xs = 0.0f;
    if (xe > (float)width)
        xe = (float)width;
    if (xe <= xs)
        return;
    int ix0 = (int)floorf(xs);
    int ix1 = (int)floorf(xe);
    int high; /* highest cell index written, for the touched-range tracker */
    if (ix0 == ix1)
    {
        s_cover[ix0] += weight * (xe - xs);
        high = ix0;
    }
    else
    {
        s_cover[ix0] += weight * ((float)(ix0 + 1) - xs);
        for (int x = ix0 + 1; x < ix1; x++)
            s_cover[x] += weight;
        if (ix1 < width)
        {
            s_cover[ix1] += weight * (xe - (float)ix1);
            high = ix1;
        }
        else
        {
            high = ix1 - 1;
        }
    }
    /* Record the span so the row's clear + emit touch only covered pixels, not the whole clip width. */
    if (ix0 < s_row_lo)
        s_row_lo = ix0;
    if (high + 1 > s_row_hi)
        s_row_hi = high + 1;
}

/** @brief Returns true when winding @p acc counts as "inside" under the given fill rule. */
static int rule_inside(int acc, int evenodd)
{
    return evenodd ? (acc & 1) : (acc != 0);
}

#if ERUI_GRADIENT
/* Vector gradient fill. Reuses gradient.c's always-compiled colour helpers, forward-declared here
 * to keep this TU decoupled from the node internals that gradient.h would pull in. */
uint32_t er_gradient_eval_stops(const ERGradientStop* stops, int count, float t);
uint32_t er_gradient_premul(uint32_t sa);

/* Per-row premultiplied scratch for a gradient fill span (sized like the coverage row). */
static uint32_t s_vgrad_row[ERUI_VECTOR_MAX_ROW];

/* Precomputed colour ramp: ERUI_VECTOR_GRAD_LUT premultiplied-ARGB entries sampled across t∈[0,1), rebuilt
 * once per gradient shape (build_grad_lut). The per-pixel sampler then indexes this instead of re-running the
 * stop search/interpolation (er_gradient_eval_stops) every pixel — the bulk of the conic dial's drag cost.
 * 256 matches 8-bit colour resolution; a RAM-tight board may lower it (a 1 KB internal buffer at 256) at the
 * cost of coarser colour steps. Tunable like the other vector pools (CMake cache var; see engine/README). */
#ifndef ERUI_VECTOR_GRAD_LUT
#define ERUI_VECTOR_GRAD_LUT 256
#endif
static uint32_t s_vgrad_lut[ERUI_VECTOR_GRAD_LUT];

/** @brief Builds the premultiplied colour LUT for a gradient (one er_gradient_eval_stops per entry). */
static void build_grad_lut(const ERVectorGradient* g)
{
    for (int i = 0; i < ERUI_VECTOR_GRAD_LUT; i++)
    {
        const float t = ((float)i + 0.5f) / (float)ERUI_VECTOR_GRAD_LUT;
        s_vgrad_lut[i] = er_gradient_premul(er_gradient_eval_stops(g->stops, g->stop_count, t));
    }
}

/** @brief Scales a premultiplied-ARGB pixel by an 8-bit coverage (all four channels), rounding to nearest. */
static inline uint32_t vgrad_scale(uint32_t p, uint32_t cov)
{
    const uint32_t a = (((p >> 24) & 0xFFU) * cov + 127U) / 255U;
    const uint32_t r = (((p >> 16) & 0xFFU) * cov + 127U) / 255U;
    const uint32_t gg = (((p >> 8) & 0xFFU) * cov + 127U) / 255U;
    const uint32_t b = ((p & 0xFFU) * cov + 127U) / 255U;
    return (a << 24) | (r << 16) | (gg << 8) | b;
}

#if ERUI_GRADIENT_CONIC
/**
 * @brief Fast atan2 approximation (max error ~0.0015 rad), to avoid the soft-float atan2f libm call in the
 *        per-pixel conic sampler. Same argument order as atan2f(y, x); range (-PI, PI]. A 256-entry colour
 *        LUT quantizes the angle to ~1.4° steps anyway, so this error is invisible.
 */
static inline float vgrad_fast_atan2(float y, float x)
{
    const float ax = fabsf(x), ay = fabsf(y);
    if (ax < 1e-12f && ay < 1e-12f)
        return 0.0f;
    const float a = (ax > ay) ? (ay / ax) : (ax / ay); /* ratio in [0,1] */
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a; /* atan(a) */
    if (ay > ax)
        r = 1.57079637f - r; /* PI/2 - r */
    if (x < 0.0f)
        r = 3.14159274f - r;
    if (y < 0.0f)
        r = -r;
    return r;
}
#endif

/** @brief True if a gradient type is supported in this build (radial gated on ERUI_GRADIENT_RADIAL). */
static int vgrad_supported(int type)
{
    if (type == ER_GRADIENT_LINEAR)
        return 1;
#if ERUI_GRADIENT_RADIAL
    if (type == ER_GRADIENT_RADIAL)
        return 1;
#endif
#if ERUI_GRADIENT_CONIC
    if (type == ER_GRADIENT_CONIC)
        return 1;
#endif
    return 0;
}

/**
 * @brief Gradient parameter t at a framebuffer pixel centre (geometry is already in framebuffer space).
 *        Linear: projection onto the axis A->B, normalized. Radial: distance from the centre over r.
 *        Conic: angle of (pixel - centre), clockwise from the top, minus the start angle, wrapped to [0,1).
 *        er_gradient_eval_stops() clamps t to the endpoint colours, so t is left unclamped here.
 */
static float vgrad_t(const ERVectorGradient* g, float fx, float fy)
{
#if ERUI_GRADIENT_RADIAL
    if (g->type == ER_GRADIENT_RADIAL)
    {
        const float dx = fx - g->ax, dy = fy - g->ay;
        return (g->r > 1e-6f) ? sqrtf(dx * dx + dy * dy) / g->r : 0.0f;
    }
#endif
#if ERUI_GRADIENT_CONIC
    if (g->type == ER_GRADIENT_CONIC)
    {
        /* atan2(dx, -dy): 0 at the top, increasing clockwise. Subtract the start angle, wrap to [0,1).
         * Uses the polynomial atan2 approximation — this runs per covered pixel on the drag hot path. */
        float a = (vgrad_fast_atan2(fx - g->ax, -(fy - g->ay)) - g->r) / (2.0f * VEC_PI);
        a -= floorf(a);
        return a;
    }
#endif
    {
        const float dx = g->bx - g->ax, dy = g->by - g->ay;
        const float len2 = dx * dx + dy * dy;
        if (len2 < 1e-6f)
            return 0.0f;
        return ((fx - g->ax) * dx + (fy - g->ay) * dy) / len2;
    }
}

/**
 * @brief Resolves a paint's 1-based gradient index into a framebuffer-space gradient (or NULL).
 *        Copies the table entry into @p buf, offsets its op-tape (node-local) geometry by the node origin
 *        (px,py) to match the flattened path, and returns @p buf — or NULL if the index is out of range,
 *        the type is unsupported in this build, or there are < 2 stops. Used for both fill and stroke.
 */
static const ERVectorGradient*
resolve_grad(int idx1, const ERVectorGradient* grads, int n_grads, int px, int py, ERVectorGradient* buf)
{
    if (idx1 <= 0 || idx1 > n_grads || !grads)
        return NULL;
    *buf = grads[idx1 - 1];
    if (!vgrad_supported(buf->type) || buf->stop_count < 2)
        return NULL;
    buf->ax += (float)px; /* r is a length and is NOT offset */
    buf->ay += (float)py;
    buf->bx += (float)px;
    buf->by += (float)py;
    return buf;
}
#endif /* ERUI_GRADIENT */

/**
 * @brief Blends the accumulated coverage row into the target, coalescing equal-alpha runs.
 *
 * Scans only [lo,hi) — the x-range cover_span() actually touched this row — and zeroes each
 * consumed cell so the next row starts clean without a full-width memset. For a thin stroke in a
 * wide clip this turns the per-row floor from O(clip width) into O(covered width).
 *
 * When @p grad is non-NULL the fill colour is sampled per pixel from the gradient (built into a
 * premultiplied row and flushed via er_blit_blend) instead of the constant @p color.
 */
static void emit_row(uint32_t color, const ERVectorGradient* grad, int iy, int clipx0, int lo, int hi)
{
#if ERUI_GRADIENT
    if (grad)
    {
        const int n = hi - lo;
        for (int xx = 0; xx < n; xx++)
        {
            float c = s_cover[lo + xx];
            s_cover[lo + xx] = 0.0f; /* consume so the cell is clean for the next row */
            if (c <= 0.0f)
            {
                s_vgrad_row[xx] = 0u; /* uncovered: alpha 0 leaves the destination untouched */
                continue;
            }
            if (c > 1.0f)
                c = 1.0f;
            /* t -> LUT index (clamped; eval clamps to the endpoint colours, so out-of-range t pins to an end).
             * The LUT entry is premultiplied, so scaling all four channels by coverage finishes the pixel —
             * no per-pixel stop interpolation or premultiply. */
            const float t = vgrad_t(grad, (float)(clipx0 + lo + xx) + 0.5f, (float)iy + 0.5f);
            int idx = (int)(t * (float)ERUI_VECTOR_GRAD_LUT);
            if (idx < 0)
                idx = 0;
            else if (idx >= ERUI_VECTOR_GRAD_LUT)
                idx = ERUI_VECTOR_GRAD_LUT - 1;
            s_vgrad_row[xx] = vgrad_scale(s_vgrad_lut[idx], (uint32_t)(c * 255.0f + 0.5f));
        }
        er_blit_blend(s_vgrad_row, (int)(sizeof(uint32_t) * (size_t)n), 255, clipx0 + lo, iy, n, 1);
        return;
    }
#else
    (void)grad;
#endif
    const uint32_t pa = (color >> 24) & 0xFFU;
    const uint32_t rgb = color & 0x00FFFFFFU;
    int x = lo;
    while (x < hi)
    {
        float c = s_cover[x];
        if (c <= 0.0f)
        {
            x++;
            continue;
        }
        s_cover[x] = 0.0f; /* consume so the cell is clean for the next row */
        if (c > 1.0f)
            c = 1.0f;
        const uint32_t a = (pa * (uint32_t)(c * 255.0f + 0.5f) + 127U) / 255U;
        /* Coalesce a run of pixels with the same quantized alpha. */
        int run = 1;
        while (x + run < hi)
        {
            float c2 = s_cover[x + run];
            if (c2 > 1.0f)
                c2 = 1.0f;
            const uint32_t a2 = c2 <= 0.0f ? 0U : (pa * (uint32_t)(c2 * 255.0f + 0.5f) + 127U) / 255U;
            if (a2 != a)
                break;
            s_cover[x + run] = 0.0f; /* consume */
            run++;
        }
        if (a != 0U)
            er_blit_fill((a << 24) | rgb, clipx0 + x, iy, run, 1);
        x += run;
    }
}

/* Indices of edges crossing the current scanline (the active-edge table). */
static int s_active[ERUI_VECTOR_MAX_EDGES];

/**
 * @brief Rasterizes an edge list into the clip box with anti-aliasing.
 *
 * @p edges is normally this file's own scratch (s_edges) just built for the pass, but a cache replay
 * hands in a recorded list instead — the pointer indirection is what lets replay skip the geometry
 * build entirely. @p n_edges must not exceed ERUI_VECTOR_MAX_EDGES (the sort/active tables are sized
 * to it); every producer — the build path and the cache recorder — enforces that cap.
 */
static void rasterize(const ERVecEdge* edges,
                      int n_edges,
                      uint32_t color,
                      const ERVectorGradient* grad,
                      int evenodd,
                      int clipx0,
                      int clipy0,
                      int clipx1,
                      int clipy1)
{
    /* Bail on a fully-transparent solid colour — unless a gradient supplies the fill colour instead. */
    if (n_edges == 0 || (!grad && ((color >> 24) & 0xFFU) == 0U))
        return;
    const int width = clipx1 - clipx0;
    if (width <= 0)
        return;
    if (width > ERUI_VECTOR_MAX_ROW)
    {
        ERUI_VEC_WARN_ONCE("ERUI_VECTOR_MAX_ROW", ERUI_VECTOR_MAX_ROW);
        return;
    }
#if ERUI_GRADIENT
    /* Build the colour LUT once per gradient shape — the per-pixel sampler (emit_row) then indexes it
     * instead of interpolating the stops every pixel. */
    if (grad)
        build_grad_lut(grad);
#endif

    float fy0 = 1e30f, fy1 = -1e30f;
    for (int i = 0; i < n_edges; i++)
    {
        if (edges[i].y0 < fy0)
            fy0 = edges[i].y0;
        if (edges[i].y1 > fy1)
            fy1 = edges[i].y1;
    }
    int ymin = (int)floorf(fy0);
    int ymax = (int)ceilf(fy1);
    if (ymin < clipy0)
        ymin = clipy0;
    if (ymax > clipy1)
        ymax = clipy1;
    if (ymax <= ymin)
        return;

    /* Order edges top-to-bottom so each scanline only tests the few edges actually crossing it (an
     * active-edge table) instead of all of them — the difference between O(edges*rows) and ~O(rows),
     * which is what makes a long arc stroke cheap to rasterize.
     *
     * The ordering is a counting sort on the edge's integer START ROW: one linear pass to histogram,
     * one to place, no comparator calls and no data movement beyond a 2-byte index — where the qsort
     * this replaces cost O(n log n) indirect calls, repeated for every fill, every stroke and every
     * damage rect the node straddles. Rows map to buckets through a shift, so a clip taller than the
     * bucket table just puts a few rows in each bucket. Buckets are activated WHOLE, in row order, so
     * edges need no ordering within one: activating an edge a row or two early is harmless — the
     * crossing test below ignores it until its own y0, and the retire pass drops it once it ends. */
    const int nrows = ymax - ymin;
    int shift = 0;
    while ((nrows >> shift) >= ERUI_VECTOR_SORT_BUCKETS)
        shift++;
    const int nb = ((nrows - 1) >> shift) + 1;
    memset(s_bkt, 0, sizeof(VecIdx) * (size_t)nb);
    for (int i = 0; i < n_edges; i++)
    {
        int row = (int)floorf(edges[i].y0) - ymin;
        if (row < 0)
            row = 0; /* starts above the clip: activate it on the first row */
        else if (row >= nrows)
            continue; /* starts below the last row scanned: it can never cross one */
        s_bkt[row >> shift]++;
    }
    int acc = 0;
    for (int b = 0; b < nb; b++)
    {
        const int c = (int)s_bkt[b];
        s_bkt[b] = (VecIdx)acc; /* bucket start; the placement pass advances it to the bucket END */
        acc += c;
    }
    if (acc == 0)
        return;
    for (int i = 0; i < n_edges; i++)
    {
        int row = (int)floorf(edges[i].y0) - ymin;
        if (row < 0)
            row = 0;
        else if (row >= nrows)
            continue;
        s_order[s_bkt[row >> shift]++] = (VecIdx)i;
    }

    const float w = 1.0f / (float)VEC_SUBSAMPLES;
    int next = 0;     /* next edge (in bucket order) not yet activated */
    int n_active = 0; /* count in s_active */

    for (int iy = ymin; iy < ymax; iy++)
    {
        /* Activate this row's bucket (and any before it); retire edges that ended above the row. */
        const int upto = (int)s_bkt[(iy - ymin) >> shift];
        while (next < upto)
            s_active[n_active++] = (int)s_order[next++];
        for (int a = 0; a < n_active;)
        {
            if (edges[s_active[a]].y1 <= (float)iy)
                s_active[a] = s_active[--n_active];
            else
                a++;
        }

        /* s_cover is left zeroed by the previous row's emit_row (which clears every cell it consumes),
         * so no full-width memset is needed; track the x-range this row actually touches instead. */
        s_row_lo = width;
        s_row_hi = 0;
        for (int s = 0; s < VEC_SUBSAMPLES; s++)
        {
            const float sy = (float)iy + ((float)s + 0.5f) * w;
            /* Collect crossings of this sub-scanline from the active set only. */
            int m = 0;
            for (int a = 0; a < n_active; a++)
            {
                const ERVecEdge* e = &edges[s_active[a]];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                const float t = (sy - e->y0) / (e->y1 - e->y0);
                s_cross_x[m] = e->x0 + (e->x1 - e->x0) * t;
                s_cross_d[m] = e->dir;
                m++;
            }
            if (m < 2)
                continue;
            /* Insertion sort crossings by x (m is small in practice). */
            for (int a = 1; a < m; a++)
            {
                const float kx = s_cross_x[a];
                const int kd = s_cross_d[a];
                int b = a - 1;
                while (b >= 0 && s_cross_x[b] > kx)
                {
                    s_cross_x[b + 1] = s_cross_x[b];
                    s_cross_d[b + 1] = s_cross_d[b];
                    b--;
                }
                s_cross_x[b + 1] = kx;
                s_cross_d[b + 1] = kd;
            }
            /* Walk crossings, accumulating winding; emit covered spans. */
            int acc = 0;
            for (int k = 0; k < m - 1; k++)
            {
                acc += s_cross_d[k];
                if (rule_inside(acc, evenodd))
                    cover_span(s_cross_x[k], s_cross_x[k + 1], w, clipx0, width);
            }
        }
        if (s_row_hi > s_row_lo)
            emit_row(color, grad, iy, clipx0, s_row_lo, s_row_hi);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Fill + stroke
 ---------------------------------------------------------------------------------------------------------------------*/

#if ERUI_VECTOR_EDGE_CACHE
/**
 * @brief Appends the just-built edge list (s_edges) to the recording entry as one replay pass.
 *
 * Stores the pass's ink bounds off the edges so replay can skip it for a clip that cannot see it —
 * the cached counterpart of shape_clipped_out (which reads the tape the replay no longer walks).
 *
 * @return false when the entry is out of pass or edge capacity (the recording must be discarded).
 */
static bool record_pass(uint8_t kind, int paint_idx)
{
    ERVecCache* e = s_rec;
    if (s_nedges == 0)
        return true; /* nothing painted, nothing to replay */
    if (e->n_passes >= ERUI_VECTOR_CACHE_PASSES || e->n_edges + s_nedges > ERUI_VECTOR_CACHE_EDGES)
    {
#if ERUI_VECTOR_DIAGNOSTICS
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            fprintf(stderr,
                    "embedded-react vector: edge cache entry full (ERUI_VECTOR_CACHE_EDGES %d / "
                    "ERUI_VECTOR_CACHE_PASSES %d) - node repaints uncached; raise them.\n",
                    (int)ERUI_VECTOR_CACHE_EDGES,
                    (int)ERUI_VECTOR_CACHE_PASSES);
        }
#endif
        return false;
    }
    ERVecPass* p = &e->passes[e->n_passes++];
    p->kind = kind;
    p->paint = (uint16_t)paint_idx;
    p->start = e->n_edges;
    p->count = s_nedges;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (int i = 0; i < s_nedges; i++)
    {
        const ERVecEdge* ed = &s_edges[i];
        const float xlo = (ed->x0 < ed->x1) ? ed->x0 : ed->x1;
        const float xhi = (ed->x0 < ed->x1) ? ed->x1 : ed->x0;
        if (xlo < bx0)
            bx0 = xlo;
        if (xhi > bx1)
            bx1 = xhi;
        if (ed->y0 < by0)
            by0 = ed->y0; /* edges are normalized y0 <= y1 */
        if (ed->y1 > by1)
            by1 = ed->y1;
    }
    p->bx0 = bx0;
    p->by0 = by0;
    p->bx1 = bx1;
    p->by1 = by1;
    memcpy(e->edges + e->n_edges, s_edges, (size_t)s_nedges * sizeof(ERVecEdge));
    e->n_edges += s_nedges;
    return true;
}
#endif /* ERUI_VECTOR_EDGE_CACHE */

/** @brief Builds closed fill edges from every subpath (into s_edges, honoring s_edge_ytop/ybot). */
static void fill_build(void)
{
    s_nedges = 0;
    s_edge_trunc = false;
    for (int si = 0; si < s_nsub; si++)
    {
        const VecSub* sp = &s_sub[si];
        if (sp->count < 2)
            continue;
        for (int i = 0; i < sp->count - 1; i++)
            edge_add(s_px[sp->start + i], s_py[sp->start + i], s_px[sp->start + i + 1], s_py[sp->start + i + 1]);
        /* Fill implicitly closes every subpath. */
        edge_add(s_px[sp->start + sp->count - 1], s_py[sp->start + sp->count - 1], s_px[sp->start], s_py[sp->start]);
    }
}

/** @brief Builds closed edges from every subpath and rasterizes the fill (recording it when armed). */
static void
fill_shape(uint32_t color, const ERVectorGradient* grad, int evenodd, int paint_idx, int cx0, int cy0, int cx1, int cy1)
{
#if ERUI_VECTOR_EDGE_CACHE
    if (VEC_RECORDING())
    {
        /* Record mode: build the FULL edge list (no clip-row drop) so any later clip can replay it. */
        s_edge_ytop = -1e30f;
        s_edge_ybot = 1e30f;
        fill_build();
        if (s_edge_trunc)
        {
            /* The unclipped build overflowed the edge pool; a clipped build may keep edges this one
             * dropped, so abandon the recording and fall through to the legacy build for THIS render. */
            s_rec_failed = true;
        }
        else
        {
            if (!record_pass(ER_VEC_PASS_FILL, paint_idx))
                s_rec_failed = true;
            /* The built edges are complete either way — rasterize them directly. */
            rasterize(s_edges, s_nedges, color, grad, evenodd, cx0, cy0, cx1, cy1);
            return;
        }
    }
#else
    (void)paint_idx;
#endif
    s_edge_ytop = (float)cy0;
    s_edge_ybot = (float)cy1;
    fill_build();
    rasterize(s_edges, s_nedges, color, grad, evenodd, cx0, cy0, cx1, cy1);
}

/** @brief Adds a filled convex quad (4 corners, CCW or CW) as edges. */
static void add_quad(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy)
{
    edge_add(ax, ay, bx, by);
    edge_add(bx, by, cx, cy);
    edge_add(cx, cy, dx, dy);
    edge_add(dx, dy, ax, ay);
}

/**
 * @brief Adds a filled disc (used for round caps) approximated by an n-gon.
 *
 * The perimeter is traversed CLOCKWISE (t decreasing) so the disc's winding sign matches the segment
 * quads from add_quad(). A round cap disc overlaps the stroke body it sits on; under the nonzero rule
 * matching signs make the overlap add (stay filled) — the opposite winding would cancel it to zero and
 * punch a half-circle hole at each arc end.
 */
static void add_disc(float cx, float cy, float r)
{
    if (r <= 0.0f)
        return;
    int n = (int)(r * 1.5f);
    if (n < 8)
        n = 8;
    if (n > 28)
        n = 28;
    float px = cx + r, py = cy;
    for (int i = 1; i <= n; i++)
    {
        const float t = -2.0f * VEC_PI * (float)i / (float)n;
        const float qx = cx + r * cosf(t);
        const float qy = cy + r * sinf(t);
        edge_add(px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

/** @brief Adds a triangle as edges (for bevel/miter joins). */
static void add_tri(float ax, float ay, float bx, float by, float cx, float cy)
{
    edge_add(ax, ay, bx, by);
    edge_add(bx, by, cx, cy);
    edge_add(cx, cy, ax, ay);
}

/** @brief Unit direction + length of a segment; false when it is degenerate (nothing to stroke). */
static bool seg_dir(float x0, float y0, float x1, float y1, float* ux, float* uy, float* len)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-6f)
        return false;
    *ux = dx / l;
    *uy = dy / l;
    *len = l;
    return true;
}

/**
 * @brief Decides whether a vertex's join can be folded into its two segment quads instead of drawn.
 *
 * Two quads butted at a vertex leave the whole pie slice between them uncovered — the join's job is to
 * fill it, and dropping one notches the stroke by the slice's width (~r * turn angle), which is why a
 * near-collinear test alone cannot skip it without leaving a visible bite. Mitering BOTH sides of the
 * shared corner instead closes the slice exactly: the two quads then meet along the angle bisector,
 * with the corner pushed out to r/cos(t/2) — and when that is within VEC_JOIN_TOL of r, the result is
 * within a sub-pixel of the round/bevel join it replaces, whichever style is set. That makes it free
 * to apply on smooth flattened curves, where it drops the per-vertex join geometry (and its trig)
 * entirely: an arc's every vertex used to pay for a triangle fan covering a gap thinner than a pixel.
 *
 * @param[in]  u0x,u0y,l0  Incoming segment direction (unit) and length.
 * @param[in]  u1x,u1y,l1  Outgoing segment direction (unit) and length.
 * @param[in]  r           Stroke half-width.
 * @param[in]  merge_dot   Minimum dot product for the miter to stay within tolerance (see caller).
 * @param[out] ox,oy       Corner offset vector: the shared corners are vertex +/- (ox,oy).
 *
 * @return true when the join merged, in which case NO join geometry is needed at this vertex.
 */
static bool join_merge(
    float u0x, float u0y, float l0, float u1x, float u1y, float l1, float r, float merge_dot, float* ox, float* oy)
{
    const float dot = u0x * u1x + u0y * u1y;
    if (dot <= 0.0f || dot < merge_dot)
        return false;
    const float cross = u0x * u1y - u0y * u1x;
    float ex = 0.0f; /* how far the corner slides ALONG the incoming segment; 0 when collinear */
    if (fabsf(cross) > 1e-6f)
    {
        ex = r * (dot - 1.0f) / cross; /* = -/+ r * tan(t/2) */
        /* Never eat more than a quarter of either segment. Both ends of a segment slide, and on a
         * zigzag they slide OPPOSITE ways (the turn flips sign), so anything looser lets a quad pinch
         * shut at one corner and drop the sliver of coverage the join used to hold. Past the limit the
         * corner simply does not merge and the join is drawn as before. */
        const float lim = 0.25f * ((l0 < l1) ? l0 : l1);
        if (ex > lim || ex < -lim)
            return false;
    }
    *ox = -u0y * r + u0x * ex;
    *oy = u0x * r + u0y * ex;
    return true;
}

/**
 * @brief Adds the join geometry at a vertex whose corner could not be merged (a real corner).
 *
 * Round joins fan the outer gap with triangles back to the vertex, bevel uses one triangle, and miter
 * extends to the intersection of the outer offset lines (falling back to bevel past the miter limit).
 *
 * @param[in] vx,vy    Vertex position.
 * @param[in] u0x,u0y  Incoming segment direction (unit).
 * @param[in] u1x,u1y  Outgoing segment direction (unit).
 * @param[in] r        Stroke half-width.
 * @param[in] join     ER_VJOIN_* style.
 * @param[in] miter    Miter limit (multiples of the half-width).
 */
static void add_join(float vx, float vy, float u0x, float u0y, float u1x, float u1y, float r, int join, float miter)
{
    /* Outer side normals + which side carries the gap. */
    const float n0x = -u0y * r, n0y = u0x * r;
    const float n1x = -u1y * r, n1y = u1x * r;
    const float cross = u0x * u1y - u0y * u1x;
    const float s = (cross < 0.0f) ? 1.0f : -1.0f; /* pick the outer offsets */
    const float p0x = vx + s * n0x, p0y = vy + s * n0y;
    const float p1x = vx + s * n1x, p1y = vy + s * n1y;

    if (join == ER_VJOIN_ROUND)
    {
        /* Fan the outer gap with triangles back to the vertex — a true round join in a handful of
         * edges (one triangle for a gentle bend), not a full disc per vertex. */
        const float a0 = atan2f(p0y - vy, p0x - vx);
        const float a1 = atan2f(p1y - vy, p1x - vx);
        float da = a1 - a0;
        while (da > VEC_PI)
            da -= 2.0f * VEC_PI;
        while (da < -VEC_PI)
            da += 2.0f * VEC_PI;
        int steps = (int)ceilf(fabsf(da) / 0.4f); /* ~23 degrees per step */
        if (steps < 1)
            steps = 1;
        float pax = p0x, pay = p0y;
        for (int k = 1; k <= steps; k++)
        {
            const float t = a0 + da * ((float)k / (float)steps);
            const float pbx = vx + r * cosf(t);
            const float pby = vy + r * sinf(t);
            add_tri(vx, vy, pax, pay, pbx, pby);
            pax = pbx;
            pay = pby;
        }
        return;
    }
    if (join == ER_VJOIN_MITER)
    {
        /* Miter point = intersection of the two outer offset lines. */
        const float denom = cross;
        if (fabsf(denom) > 1e-6f)
        {
            const float t = ((p1x - p0x) * u1y - (p1y - p0y) * u1x) / denom;
            const float mx = p0x + u0x * t;
            const float my = p0y + u0y * t;
            const float mdx = mx - vx, mdy = my - vy;
            const float mlen = sqrtf(mdx * mdx + mdy * mdy);
            if (mlen <= miter * r)
            {
                add_tri(vx, vy, p0x, p0y, mx, my);
                add_tri(vx, vy, mx, my, p1x, p1y);
                return;
            }
        }
        /* Too sharp -> fall through to bevel. */
    }
    add_tri(vx, vy, p0x, p0y, p1x, p1y); /* bevel */
}

/** @brief Vertex a segment ends at. Only a closing segment (k == n-1) wraps back to the start. */
static inline int seg_end_index(int k, int n)
{
    return (k + 1 == n) ? 0 : k + 1;
}

/**
 * @brief Builds stroke outline geometry for one subpath and adds it to the edge list.
 *
 * Each segment becomes a quad of width @p sw and open endpoints get a cap. At an interior vertex the
 * two quads either share a mitered corner (join_merge, the common case on a flattened curve) or butt
 * against each other and the gap between them is filled by join geometry (add_join). The pieces
 * overlap and are unioned by the nonzero fill rule, so no single clean outline polygon is needed.
 *
 * A closed subpath whose CLOSE does not land back on its start point gets one extra segment for that
 * closing edge, so the segment list wraps and its two ends are corners like any other.
 */
static void stroke_subpath(const VecSub* sp, float sw, int cap, int join, float miter)
{
    const int n = sp->count;
    if (n < 1)
        return;
    const float r = sw * 0.5f;
    const float* X = &s_px[sp->start];
    const float* Y = &s_py[sp->start];

    if (n == 1)
    {
        if (cap == ER_VCAP_ROUND)
            add_disc(X[0], Y[0], r);
        return;
    }

    const int closed = sp->closed;

    /* Merge a corner while the miter it produces stays within VEC_JOIN_TOL of the half-width:
     * |corner| = r / cos(t/2), so the bound is cos(t/2) >= r / (r + tol), and dot = 2cos²(t/2) - 1. */
    const float mk = r / (r + VEC_JOIN_TOL);
    const float merge_dot = 2.0f * mk * mk - 1.0f;

    /* A CLOSE does not have to bring the path back to where it started, and when it does not there is
     * still an edge from the last vertex to the first. The fill closes every subpath implicitly, so
     * leaving that edge out of the stroke shows up as one unstroked side — a ring sector's radial edge
     * is the usual way to hit it. Give it a segment of its own; the list then wraps, and the vertices
     * at both of its ends become ordinary corners. */
    const float gapx = X[n - 1] - X[0], gapy = Y[n - 1] - Y[0];
    const bool close_seg = closed && (gapx * gapx + gapy * gapy > VEC_CLOSE_EPS * VEC_CLOSE_EPS);
    const int nseg = closed ? (close_seg ? n : n - 1) : (n - 1);

    /* The corner where the LAST segment meets segment 0 is shared by two quads built at opposite ends
     * of the loop, so it has to be decided before segment 0 is emitted. */
    bool wrap_merged = false;
    float wrap_ox = 0.0f, wrap_oy = 0.0f;
    float wrap_u1x = 0.0f, wrap_u1y = 0.0f;
    bool wrap_join = false; /* the wrap corner did not merge: its join is emitted with the last quad */
    const int wrapv = close_seg ? 0 : (n - 1); /* the shared vertex: P[0] once the list wraps */
    if (closed && nseg >= 2)
    {
        /* The outgoing direction leaves the shared vertex toward P[1]. With a closing segment that
         * vertex IS P[0], so this is segment 0's own direction; without one it is P[n-1], which sits on
         * top of P[0] — and reading it from there is what the per-vertex join has always done. */
        const int la = nseg - 1;
        const int lb = seg_end_index(la, n);
        float u0x, u0y, l0, u1x, u1y, l1;
        if (seg_dir(X[la], Y[la], X[lb], Y[lb], &u0x, &u0y, &l0)
            && seg_dir(X[wrapv], Y[wrapv], X[1], Y[1], &u1x, &u1y, &l1))
        {
            wrap_u1x = u1x;
            wrap_u1y = u1y;
            wrap_merged = join_merge(u0x, u0y, l0, u1x, u1y, l1, r, merge_dot, &wrap_ox, &wrap_oy);
            wrap_join = !wrap_merged;
        }
    }

    /* Corner carried from the previous vertex; segment 0 of a closed subpath inherits the wrap corner
     * so both quads meeting there use the same one. */
    bool pmerged = wrap_merged;
    float pox = wrap_ox, poy = wrap_oy;

    for (int k = 0; k < nseg; k++)
    {
        const int ia = k, ib = seg_end_index(k, n);
        float ux, uy, len;
        if (!seg_dir(X[ia], Y[ia], X[ib], Y[ib], &ux, &uy, &len))
        {
            pmerged = false; /* nothing was emitted here, so the next vertex starts clean */
            continue;
        }
        const float nx = -uy * r, ny = ux * r; /* left normal * r */
        float ax = X[ia], ay = Y[ia], bx = X[ib], by = Y[ib];
        /* Square cap: extend the open ends by r along the segment direction. */
        if (cap == ER_VCAP_SQUARE && !closed)
        {
            if (k == 0)
            {
                ax -= ux * r;
                ay -= uy * r;
            }
            if (k == nseg - 1)
            {
                bx += ux * r;
                by += uy * r;
            }
        }

        /* The corner at this segment's END: shared with the next segment (or, on a closed subpath's
         * last segment, with segment 0 — decided up front so both agree). */
        bool merged = false;
        float ox = 0.0f, oy = 0.0f;
        if (closed && k == nseg - 1)
        {
            merged = wrap_merged;
            ox = wrap_ox;
            oy = wrap_oy;
            if (wrap_join)
                add_join(X[ib], Y[ib], ux, uy, wrap_u1x, wrap_u1y, r, join, miter);
        }
        else if (k < nseg - 1)
        {
            const int ic = seg_end_index(k + 1, n);
            float vx, vy, vlen;
            if (seg_dir(X[ib], Y[ib], X[ic], Y[ic], &vx, &vy, &vlen))
            {
                merged = join_merge(ux, uy, len, vx, vy, vlen, r, merge_dot, &ox, &oy);
                if (!merged)
                    add_join(X[ib], Y[ib], ux, uy, vx, vy, r, join, miter);
            }
        }

        add_quad(pmerged ? ax + pox : ax + nx,
                 pmerged ? ay + poy : ay + ny,
                 merged ? bx + ox : bx + nx,
                 merged ? by + oy : by + ny,
                 merged ? bx - ox : bx - nx,
                 merged ? by - oy : by - ny,
                 pmerged ? ax - pox : ax - nx,
                 pmerged ? ay - poy : ay - ny);

        pmerged = merged;
        pox = ox;
        poy = oy;
    }

    /* Caps at open endpoints. */
    if (!closed && cap == ER_VCAP_ROUND)
    {
        add_disc(X[0], Y[0], r);
        add_disc(X[n - 1], Y[n - 1], r);
    }
}

/** @brief Builds stroke outline edges for every subpath (into s_edges, honoring s_edge_ytop/ybot). */
static void stroke_build(float sw, int cap, int join, float miter)
{
    s_nedges = 0;
    s_edge_trunc = false;
    for (int si = 0; si < s_nsub; si++)
        if (s_sub[si].count >= 1)
            stroke_subpath(&s_sub[si], sw, cap, join, miter);
}

/** @brief Builds stroke geometry for all subpaths and rasterizes it (recording it when armed). */
static void stroke_shape(uint32_t color,
                         const ERVectorGradient* grad,
                         float sw,
                         int cap,
                         int join,
                         float miter,
                         int paint_idx,
                         int cx0,
                         int cy0,
                         int cx1,
                         int cy1)
{
    if (sw <= 0.0f)
        return;
#if ERUI_VECTOR_EDGE_CACHE
    if (VEC_RECORDING())
    {
        /* Record mode: full edge list, no clip-row drop — see fill_shape for the overflow contract. */
        s_edge_ytop = -1e30f;
        s_edge_ybot = 1e30f;
        stroke_build(sw, cap, join, miter);
        if (s_edge_trunc)
        {
            s_rec_failed = true;
        }
        else
        {
            if (!record_pass(ER_VEC_PASS_STROKE, paint_idx))
                s_rec_failed = true;
            rasterize(s_edges, s_nedges, color, grad, 0, cx0, cy0, cx1, cy1);
            return;
        }
    }
#else
    (void)paint_idx;
#endif
    s_edge_ytop = (float)cy0;
    s_edge_ybot = (float)cy1;
    stroke_build(sw, cap, join, miter);
    rasterize(s_edges, s_nedges, color, grad, 0, cx0, cy0, cx1, cy1);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Clip rejection
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief How far a paint's ink can reach outside the path itself, in pixels.
 *
 * Half the stroke width, extended for the join style (a miter runs out to miter*half-width) and for
 * square caps, plus a pixel of anti-aliasing slack.
 *
 * The stroke counts whenever one will be DRAWN, which is not the same as the stroke colour being
 * visible: a gradient stroke paints from the gradient table and leaves the solid colour transparent
 * (see the stroke_shape call in er_vector_render). Reading only the colour there would hand back a
 * 1 px margin for a wide gradient stroke and let clip rejection drop a shape whose ink reaches well
 * inside the clip.
 */
static float paint_margin(const ERVectorPaint* pt)
{
    if (!pt)
        return 1.0f;
    float m = 1.0f;
    const bool has_stroke = pt->stroke_w > 0.0f && (((pt->stroke >> 24) & 0xFFU) != 0U || pt->stroke_grad != 0);
    if (has_stroke)
    {
        const float hw = pt->stroke_w * 0.5f;
        float k = 1.5f; /* round/butt/square caps and bevel joins all stay inside 1.5 * half-width */
        if (pt->join == ER_VJOIN_MITER)
        {
            const float ml = (pt->miter > 0.0f) ? pt->miter : 4.0f;
            if (ml > k)
                k = ml;
        }
        m += hw * k;
    }
    return m;
}

/**
 * @brief Decides whether a shape's ink cannot reach the clip box, reading only its op run.
 *
 * Bounds the run as it walks it — curves by their control points (a bezier stays inside its control
 * hull), arcs by the centre square — so it needs no flattening and no trig, just a few compares per
 * op. The bound only ever grows, so the moment it reaches the clip the answer is settled and the walk
 * stops: a shape that IS on screen costs a couple of ops, and only one being skipped is walked whole.
 *
 * @param[in]  ops    Op-tape.
 * @param[in]  i      Index of the run's first op (just past SHAPE + its paint index).
 * @param[in]  n_ops  Tape length.
 * @param[in]  px,py  Node origin added to tape coordinates.
 * @param[in]  m      Margin the paint adds around the path (paint_margin).
 * @param[in]  cx0,cy0,cx1,cy1  Clip box.
 * @param[out] end    Receives the tape index just past the run (only meaningful when skipping).
 *
 * @return true when the shape cannot touch the clip and may be skipped whole. False is always safe:
 *         it is also the answer for a truncated or unknown op, which the general parser then handles.
 */
static bool shape_clipped_out(
    const float* ops, int i, int n_ops, int px, int py, float m, int cx0, int cy0, int cx1, int cy1, int* end)
{
    const float fx0 = (float)cx0 - m, fy0 = (float)cy0 - m;
    const float fx1 = (float)cx1 + m, fy1 = (float)cy1 + m;
    bool any = false;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;

    while (i < n_ops && ops[i] != ER_VOP_SHAPE)
    {
        const float op = ops[i++];
        const int op_args = (op == ER_VOP_MOVE || op == ER_VOP_LINE)   ? 2
                            : (op == ER_VOP_QUAD)                      ? 4
                            : (op == ER_VOP_CUBIC || op == ER_VOP_ARC) ? 6
                            : (op == ER_VOP_CLOSE)                     ? 0
                                                                       : -1;
        if (op_args < 0 || i + op_args > n_ops)
            return false; /* unknown or truncated: let the general parser deal with it */
        if (op == ER_VOP_ARC)
        {
            /* Bound the whole circle: cheaper than working out which quadrants the sweep touches, and
             * a partial arc is only ever a sub-box of it. */
            const float acx = (float)px + ops[i];
            const float acy = (float)py + ops[i + 1];
            const float ar = fabsf(ops[i + 2]);
            if (acx - ar < bx0)
                bx0 = acx - ar;
            if (acy - ar < by0)
                by0 = acy - ar;
            if (acx + ar > bx1)
                bx1 = acx + ar;
            if (acy + ar > by1)
                by1 = acy + ar;
            any = true;
        }
        else
        {
            for (int k = 0; k < op_args; k += 2)
            {
                const float x = (float)px + ops[i + k];
                const float y = (float)py + ops[i + k + 1];
                if (x < bx0)
                    bx0 = x;
                if (y < by0)
                    by0 = y;
                if (x > bx1)
                    bx1 = x;
                if (y > by1)
                    by1 = y;
                any = true;
            }
        }
        i += op_args;
        /* The box only grows: once it overlaps the clip, no later op can make this shape skippable. */
        if (any && bx1 > fx0 && bx0 < fx1 && by1 > fy0 && by0 < fy1)
            return false;
    }

    *end = i;
    return any;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Public entry
 ---------------------------------------------------------------------------------------------------------------------*/

#if ERUI_VECTOR_ANALYTIC_ARC

/**
 * @brief A shape's op-run recognised as one plain circular arc — the tape both flows emit for
 *        `<Arc>` (a bare ARC) and `<Circle>` (MOVE, ARC 0..2PI, CLOSE).
 */
typedef struct
{
    float cx, cy;    /**< Centre in framebuffer pixels (the node origin already folded in). */
    float r;         /**< Radius in pixels. */
    float a0_deg;    /**< Sweep start, degrees clockwise from +X. */
    float sweep_deg; /**< Sweep extent, (0, 360]. */
    bool full;       /**< Sweep covers the whole circle. */
    int end;         /**< Tape index just past this shape's op run. */
} VecArcRun;

/**
 * @brief Matches a shape's op run against the single-arc pattern.
 *
 * Accepts, in order: an optional MOVE, exactly one ARC, an optional CLOSE — and nothing else before the
 * next SHAPE / the end of the tape. A leading MOVE must land on the arc's own start point (the redundant
 * MOVE `<Circle>` emits); one that does NOT would make the general path draw a connecting line into the
 * arc, which no sector can express, so it falls back. The epsilon also absorbs the ULP-level mismatch a
 * float-rounded MOVE can carry, which the general path's exact-equality vertex dedupe misses.
 *
 * @param[in]  ops    Op-tape.
 * @param[in]  i      Index of the run's first op (just past SHAPE + its paint index).
 * @param[in]  n_ops  Tape length.
 * @param[in]  px,py  Node origin added to tape coordinates.
 * @param[out] out    Receives the arc on a match.
 *
 * @return true when the run is exactly one arc.
 */
static bool arc_run_match(const float* ops, int i, int n_ops, int px, int py, VecArcRun* out)
{
    bool have_move = false, have_arc = false;
    float mx = 0.0f, my = 0.0f, sx = 0.0f, sy = 0.0f;

    while (i < n_ops && ops[i] != ER_VOP_SHAPE)
    {
        const float op = ops[i];
        if (op == ER_VOP_MOVE)
        {
            if (have_move || have_arc || i + 3 > n_ops)
                return false; /* a second subpath, or a MOVE after the arc: not a lone arc */
            mx = (float)px + ops[i + 1];
            my = (float)py + ops[i + 2];
            have_move = true;
            i += 3;
        }
        else if (op == ER_VOP_ARC)
        {
            if (have_arc || i + 7 > n_ops)
                return false;
            const float acx = (float)px + ops[i + 1];
            const float acy = (float)py + ops[i + 2];
            float ar = ops[i + 3];
            const float a0 = ops[i + 4];
            const float a1 = ops[i + 5];
            const bool ccw = (ops[i + 6] != 0.0f);
            if (ar < 0.0f)
                ar = -ar;

            /* Normalise to a forward (clockwise, increasing-angle) sweep, matching append_arc's wrap. */
            float da = a1 - a0;
            if (ccw)
            {
                while (da > 0.0f)
                    da -= 2.0f * VEC_PI;
            }
            else
            {
                while (da < 0.0f)
                    da += 2.0f * VEC_PI;
            }
            const float sweep = (da < 0.0f) ? -da : da;
            if (!(sweep > 0.0f))
                return false; /* degenerate: nothing to draw analytically */
            out->cx = acx;
            out->cy = acy;
            out->r = ar;
            out->a0_deg = ((da < 0.0f) ? (a0 + da) : a0) * VEC_RAD2DEG;
            out->sweep_deg = sweep * VEC_RAD2DEG;
            out->full = (sweep >= 2.0f * VEC_PI - 1e-4f);
            sx = acx + ar * cosf(a0);
            sy = acy + ar * sinf(a0);
            have_arc = true;
            i += 7;
        }
        else if (op == ER_VOP_CLOSE)
        {
            if (!have_arc)
                return false;
            i += 1;
        }
        else
        {
            return false; /* a line/curve joins the arc — the general path owns joins and end caps */
        }
    }

    if (!have_arc)
        return false;
    if (have_move && (fabsf(mx - sx) > 0.01f || fabsf(my - sy) > 0.01f))
        return false;
    out->end = i;
    return true;
}

/**
 * @brief Whether a paint on a matched arc run maps exactly onto the analytic core.
 *
 * @param[in] pt  The shape's paint.
 * @param[in] a   The matched arc.
 *
 * @return true when both the fill and the stroke (whichever are present) can be drawn as sectors.
 */
static bool arc_paint_ok(const ERVectorPaint* pt, const VecArcRun* a)
{
    if (!pt || a->r <= 0.0f)
        return false;
    const bool has_fill = ((pt->fill >> 24) & 0xFFU) != 0U;
    const bool has_stroke = ((pt->stroke >> 24) & 0xFFU) != 0U && pt->stroke_w > 0.0f;
    if (!has_fill && !has_stroke)
        return false; /* nothing to draw; let the general path no-op */
    /* A filled PARTIAL arc closes on a CHORD (a circular segment), while the analytic core draws a
     * SECTOR — different shapes. Only a full circle's fill (a plain disc) maps. */
    if (has_fill && !a->full)
        return false;
    if (has_stroke)
    {
        if (pt->cap == ER_VCAP_SQUARE)
            return false; /* the analytic core has butt and round caps only */
        if (pt->stroke_w * 0.5f >= a->r)
            return false; /* the band swallows the centre and self-intersects — keep the outline path */
    }
    return true;
}

/** @brief Paints a matched arc run through the analytic core: fill (disc) then stroke (ring). */
static void arc_run_draw(const VecArcRun* a, const ERVectorPaint* pt, int cx0, int cy0, int cx1, int cy1)
{
    ERArcSector s;
    memset(&s, 0, sizeof(s));
    s.cx = a->cx;
    s.cy = a->cy;
    s.a0 = a->a0_deg;
    s.a1 = a->a0_deg + a->sweep_deg;
    s.clip_x0 = cx0;
    s.clip_y0 = cy0;
    s.clip_x1 = cx1;
    s.clip_y1 = cy1;

    if (((pt->fill >> 24) & 0xFFU) != 0U)
    {
        s.r_outer = a->r; /* reached only when full (arc_paint_ok) → a disc */
        s.r_inner = 0.0f;
        s.cap = ER_ARC_CAP_BUTT;
        s.color = pt->fill;
        er_arc_fill_sector(&s);
    }
    if (((pt->stroke >> 24) & 0xFFU) != 0U && pt->stroke_w > 0.0f)
    {
        const float hw = pt->stroke_w * 0.5f;
        s.r_outer = a->r + hw;
        s.r_inner = a->r - hw;
        s.cap = (pt->cap == ER_VCAP_ROUND) ? ER_ARC_CAP_ROUND : ER_ARC_CAP_BUTT;
        s.color = pt->stroke;
        er_arc_fill_sector(&s);
    }
}

static uint32_t s_analytic_arcs = 0U;

#if ERUI_VECTOR_EDGE_CACHE
/**
 * @brief Records a matched analytic-arc run as a replay pass (no edges — the sector core re-derives
 *        its geometry from these few parameters, and is already cheap under a small clip).
 *
 * @return false when the entry is out of pass capacity (the recording must be discarded).
 */
static bool record_arc(const VecArcRun* a, int paint_idx, const ERVectorPaint* pt)
{
    ERVecCache* e = s_rec;
    if (e->n_passes >= ERUI_VECTOR_CACHE_PASSES)
        return false;
    ERVecPass* p = &e->passes[e->n_passes++];
    p->kind = ER_VEC_PASS_ARC;
    p->paint = (uint16_t)paint_idx;
    p->start = 0;
    p->count = 0;
    /* Ink reach: the radius, plus half the stroke width when one is drawn (arc_paint_ok already
     * limited caps/joins to styles that stay inside it). */
    float reach = a->r;
    if (((pt->stroke >> 24) & 0xFFU) != 0U && pt->stroke_w > 0.0f)
        reach += pt->stroke_w * 0.5f;
    p->bx0 = a->cx - reach;
    p->by0 = a->cy - reach;
    p->bx1 = a->cx + reach;
    p->by1 = a->cy + reach;
    p->arc_cx = a->cx;
    p->arc_cy = a->cy;
    p->arc_r = a->r;
    p->arc_a0_deg = a->a0_deg;
    p->arc_sweep_deg = a->sweep_deg;
    p->arc_full = a->full;
    return true;
}
#endif /* ERUI_VECTOR_EDGE_CACHE */

#endif /* ERUI_VECTOR_ANALYTIC_ARC */

uint32_t er_vector_analytic_arc_count(void)
{
#if ERUI_VECTOR_ANALYTIC_ARC
    return s_analytic_arcs;
#else
    return 0U;
#endif
}

void er_vector_analytic_arc_count_reset(void)
{
#if ERUI_VECTOR_ANALYTIC_ARC
    s_analytic_arcs = 0U;
#endif
}

/** @brief Walks an op-tape and paints it — er_vector_render's body, shared with the recording path. */
static void render_tape(const float* ops,
                        int n_ops,
                        const ERVectorPaint* paints,
                        int n_paints,
                        const ERVectorGradient* grads,
                        int n_grads,
                        int px,
                        int py,
                        int clipx0,
                        int clipy0,
                        int clipx1,
                        int clipy1)
{
    if (!ops || n_ops <= 0)
        return;
#if !ERUI_GRADIENT
    (void)grads;
    (void)n_grads;
#endif
    /* px,py position the geometry; the clip box bounds the rasterize compute + painting (a sub-region
     * for an interactive update, or the full node box otherwise). */
    const int cx0 = clipx0, cy0 = clipy0, cx1 = clipx1, cy1 = clipy1;

    int i = 0;
    while (i < n_ops)
    {
        if (ops[i] != ER_VOP_SHAPE)
        {
            i++; /* tape must start with SHAPE; skip stray values defensively */
            continue;
        }
        i++; /* consume SHAPE opcode */
        const int pidx = (i < n_ops) ? (int)ops[i++] : 0;
        const ERVectorPaint* shape_paint = (pidx >= 0 && pidx < n_paints) ? &paints[pidx] : 0;

        /* Clip reject: bound the shape off the tape and drop it whole when its ink cannot reach the
         * clip box. A few compares per op replaces flattening it, expanding its stroke outline, sorting
         * the edges and walking every row — all of which the rasterizer would otherwise do before the
         * clip threw the result away. This is what makes a tape of many small shapes (tick marks,
         * segments, a legend) cheap to repaint for a damage rect covering only one of them.
         * OFF while recording into the edge cache: the cache must hold every shape, whatever this
         * render's clip — replay does the equivalent skip from each pass's stored bounds. */
        if (!VEC_RECORDING())
        {
            int bend = 0;
            if (shape_clipped_out(ops, i, n_ops, px, py, paint_margin(shape_paint), cx0, cy0, cx1, cy1, &bend))
            {
                i = bend;
                continue;
            }
        }

#if ERUI_VECTOR_ANALYTIC_ARC
        /* Analytic fast path: a shape that is just a circular arc goes straight to the shared sector
         * core, skipping flatten + stroke-outline + scanline coverage entirely. A gradient paint keeps
         * the general path — vector gradients carry SVG axis geometry, which the sector core's
         * angle/thickness ramps do not express. */
        if (shape_paint && shape_paint->fill_grad == 0 && shape_paint->stroke_grad == 0)
        {
            VecArcRun arun;
            if (arc_run_match(ops, i, n_ops, px, py, &arun) && arc_paint_ok(shape_paint, &arun))
            {
#if ERUI_VECTOR_EDGE_CACHE
                if (VEC_RECORDING() && !record_arc(&arun, pidx, shape_paint))
                    s_rec_failed = true;
#endif
                arc_run_draw(&arun, shape_paint, cx0, cy0, cx1, cy1);
                s_analytic_arcs++;
                i = arun.end;
                continue;
            }
        }
#endif

        /* Build this shape's subpaths until the next SHAPE or end of tape. */
        s_npts = 0;
        s_nsub = 0;
        float cur_x = 0.0f, cur_y = 0.0f;
        while (i < n_ops && ops[i] != ER_VOP_SHAPE)
        {
            const float op = ops[i++];
            /* Verify this op's args fit within the tape before reading them. A tape truncated mid-op —
             * e.g. the JS bridge capping it at ERUI_VECTOR_TAPE_MAX, or any malformed input — would
             * otherwise read past n_ops into adjacent memory, producing NaN/garbage coordinates that
             * index s_cover[] out of bounds and crash. Stop parsing the shape's tail instead. */
            const int op_args = (op == ER_VOP_MOVE || op == ER_VOP_LINE)   ? 2
                                : (op == ER_VOP_QUAD)                      ? 4
                                : (op == ER_VOP_CUBIC || op == ER_VOP_ARC) ? 6
                                                                           : 0;
            if (i + op_args > n_ops)
                break;
            if (op == ER_VOP_MOVE)
            {
                cur_x = (float)px + ops[i++];
                cur_y = (float)py + ops[i++];
                sub_begin(cur_x, cur_y);
            }
            else if (op == ER_VOP_LINE)
            {
                cur_x = (float)px + ops[i++];
                cur_y = (float)py + ops[i++];
                pt_add(cur_x, cur_y);
            }
            else if (op == ER_VOP_QUAD)
            {
                const float qcx = (float)px + ops[i++];
                const float qcy = (float)py + ops[i++];
                const float ex = (float)px + ops[i++];
                const float ey = (float)py + ops[i++];
                /* Elevate the quadratic to a cubic and reuse the cubic flattener. */
                const float c1x = cur_x + (2.0f / 3.0f) * (qcx - cur_x);
                const float c1y = cur_y + (2.0f / 3.0f) * (qcy - cur_y);
                const float c2x = ex + (2.0f / 3.0f) * (qcx - ex);
                const float c2y = ey + (2.0f / 3.0f) * (qcy - ey);
                flatten_cubic(cur_x, cur_y, c1x, c1y, c2x, c2y, ex, ey, 0);
                cur_x = ex;
                cur_y = ey;
            }
            else if (op == ER_VOP_CUBIC)
            {
                const float c1x = (float)px + ops[i++];
                const float c1y = (float)py + ops[i++];
                const float c2x = (float)px + ops[i++];
                const float c2y = (float)py + ops[i++];
                const float ex = (float)px + ops[i++];
                const float ey = (float)py + ops[i++];
                flatten_cubic(cur_x, cur_y, c1x, c1y, c2x, c2y, ex, ey, 0);
                cur_x = ex;
                cur_y = ey;
            }
            else if (op == ER_VOP_ARC)
            {
                const float acx = (float)px + ops[i++];
                const float acy = (float)py + ops[i++];
                const float ar = ops[i++];
                const float a0 = ops[i++];
                const float a1 = ops[i++];
                const int ccw = (ops[i++] != 0.0f);
                if (s_nsub == 0)
                    sub_begin(acx + ar * cosf(a0), acy + ar * sinf(a0));
                append_arc(acx, acy, ar, a0, a1, ccw);
                cur_x = acx + ar * cosf(a1);
                cur_y = acy + ar * sinf(a1);
            }
            else if (op == ER_VOP_CLOSE)
            {
                if (s_nsub > 0)
                {
                    s_sub[s_nsub - 1].closed = 1;
                    cur_x = s_px[s_sub[s_nsub - 1].start];
                    cur_y = s_py[s_sub[s_nsub - 1].start];
                }
            }
            else
            {
                /* Unknown opcode: stop to avoid desync. */
                break;
            }
        }

        /* Apply the shape's paint: fill first, then stroke (SVG paint order). */
        const ERVectorPaint* pt = shape_paint;
        if (pt)
        {
            const ERVectorGradient* fg = NULL;
            const ERVectorGradient* sg = NULL;
#if ERUI_GRADIENT
            ERVectorGradient fgbuf, sgbuf;
            fg = resolve_grad(pt->fill_grad, grads, n_grads, px, py, &fgbuf);
            sg = resolve_grad(pt->stroke_grad, grads, n_grads, px, py, &sgbuf);
#endif
            if (fg || ((pt->fill >> 24) & 0xFFU) != 0U)
                fill_shape(pt->fill, fg, pt->fill_rule == ER_VFILL_EVENODD, pidx, cx0, cy0, cx1, cy1);
            if ((sg || ((pt->stroke >> 24) & 0xFFU) != 0U) && pt->stroke_w > 0.0f)
                stroke_shape(pt->stroke,
                             sg,
                             pt->stroke_w,
                             pt->cap,
                             pt->join,
                             pt->miter > 0.0f ? pt->miter : 4.0f,
                             pidx,
                             cx0,
                             cy0,
                             cx1,
                             cy1);
        }
    }
}

void er_vector_render(const float* ops,
                      int n_ops,
                      const ERVectorPaint* paints,
                      int n_paints,
                      const ERVectorGradient* grads,
                      int n_grads,
                      int px,
                      int py,
                      int clipx0,
                      int clipy0,
                      int clipx1,
                      int clipy1)
{
    /* The tape-only entry point has no node identity to key a cache on, so it never records. */
    render_tape(ops, n_ops, paints, n_paints, grads, n_grads, px, py, clipx0, clipy0, clipx1, clipy1);
}

#if ERUI_VECTOR_EDGE_CACHE
/**
 * @brief Paints a cache entry: replays each recorded pass whose ink bounds reach the clip.
 *
 * Colors, gradients and fill rules come from the CURRENT paint table, not the recording — the store
 * invalidates the entry on any tape/paint change, so they are the same ones the recording saw; reading
 * them fresh just avoids duplicating paint state into the entry.
 */
static void replay_cache(const ERVecCache* e,
                         const ERVectorPaint* paints,
                         int n_paints,
                         const ERVectorGradient* grads,
                         int n_grads,
                         int px,
                         int py,
                         int cx0,
                         int cy0,
                         int cx1,
                         int cy1)
{
#if !ERUI_GRADIENT
    (void)grads;
    (void)n_grads;
    (void)px;
    (void)py;
#endif
    /* One pixel of slack around the stored bounds: coverage rounds outward to pixel cells. */
    const float fx0 = (float)cx0 - 1.0f, fy0 = (float)cy0 - 1.0f;
    const float fx1 = (float)cx1 + 1.0f, fy1 = (float)cy1 + 1.0f;
    for (int k = 0; k < e->n_passes; k++)
    {
        const ERVecPass* p = &e->passes[k];
        if (p->bx1 <= fx0 || p->bx0 >= fx1 || p->by1 <= fy0 || p->by0 >= fy1)
            continue; /* this pass cannot touch the clip — the cached form of shape_clipped_out */
        if ((int)p->paint >= n_paints)
            continue; /* unreachable while invalidation is sound; never index past the table */
        const ERVectorPaint* pt = &paints[p->paint];
        if (p->kind == ER_VEC_PASS_ARC)
        {
#if ERUI_VECTOR_ANALYTIC_ARC
            VecArcRun a;
            a.cx = p->arc_cx;
            a.cy = p->arc_cy;
            a.r = p->arc_r;
            a.a0_deg = p->arc_a0_deg;
            a.sweep_deg = p->arc_sweep_deg;
            a.full = p->arc_full;
            a.end = 0; /* tape position; meaningless on replay */
            arc_run_draw(&a, pt, cx0, cy0, cx1, cy1);
            s_analytic_arcs++;
#endif
            continue;
        }
        const ERVectorGradient* g = NULL;
#if ERUI_GRADIENT
        ERVectorGradient gbuf;
        g = resolve_grad(
            (p->kind == ER_VEC_PASS_FILL) ? pt->fill_grad : pt->stroke_grad, grads, n_grads, px, py, &gbuf);
#endif
        if (p->kind == ER_VEC_PASS_FILL)
            rasterize(
                e->edges + p->start, p->count, pt->fill, g, pt->fill_rule == ER_VFILL_EVENODD, cx0, cy0, cx1, cy1);
        else
            rasterize(e->edges + p->start, p->count, pt->stroke, g, 0, cx0, cy0, cx1, cy1);
    }
}
#endif /* ERUI_VECTOR_EDGE_CACHE */

void er_vector_render_slot(int slot, int px, int py, int clipx0, int clipy0, int clipx1, int clipy1)
{
    int no = 0, np = 0, ng = 0;
    const float* ops = er_vector_slot_ops(slot, &no);
    const ERVectorPaint* paints = er_vector_slot_paints(slot, &np);
    const ERVectorGradient* grads = er_vector_slot_grads(slot, &ng);
    if (!ops || no <= 0)
        return;
    if (clipx1 <= clipx0 || clipy1 <= clipy0)
        return;
#if ERUI_VECTOR_EDGE_CACHE
    const ERVecCache* hit = er_vector_cache_lookup(slot, px, py);
    if (hit)
    {
        replay_cache(hit, paints, np, grads, ng, px, py, clipx0, clipy0, clipx1, clipy1);
        return;
    }
    /* Miss. Maybe record this render — begin() only grants an entry for a tape that stayed unchanged
     * since the previous render (two-touch), so an animated node never pays the record-mode build. */
    s_rec = er_vector_cache_begin(slot, px, py);
    s_rec_failed = false;
    render_tape(ops, no, paints, np, grads, ng, px, py, clipx0, clipy0, clipx1, clipy1);
    if (s_rec)
    {
        er_vector_cache_finish(s_rec, !s_rec_failed);
        s_rec = 0;
    }
#else
    render_tape(ops, no, paints, np, grads, ng, px, py, clipx0, clipy0, clipx1, clipy1);
#endif
}
