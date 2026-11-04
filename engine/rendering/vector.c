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
 *      winding to get covered spans, accumulate fractional horizontal coverage, then stage the row as
 *      premultiplied ARGB and blend it out one call per covered stretch (which composites — incl. into
 *      the RGB565 framebuffer).
 *
 * All working buffers are static and bounded (no allocation); the engine is single-threaded so reuse
 * across shapes is safe. Coordinates from the tape are node-local; the node's screen origin (origin_x, origin_y) is
 * added during flattening, and all painting is clipped to the node box.
 */

#include "vector.h"

#include "vector_cache.h" /* ERVecEdge + the cache entry layout (record/replay share it with vector_cache.c) */

#include "arc.h"               /* er_arc_fill_sector — the shared analytic core (ERUI_VECTOR_ANALYTIC_ARC) */
#include "renderer_internal.h" /* er_blit_fill, er_blit_blend */

#include <math.h>
#include <stdbool.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Tunables
 ---------------------------------------------------------------------------------------------------------------------*/

/* These back STATIC working buffers that the rasterizer touches per-pixel/per-scanline, so on a
 * PSRAM-bss target (ESP32) they must stay in INTERNAL RAM (see the engine component's linker fragment)
 * — sized to fit there. Raise them (and the internal-RAM budget) for very complex paths. */
#ifndef ERUI_VECTOR_MAX_POINTS
#define ERUI_VECTOR_MAX_POINTS 2048 /**< Flattened polyline vertices per shape. */
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

#define VEC_SUBSAMPLES 4             /**< Vertical AA sub-scanlines per pixel row. */
#define VEC_FLATNESS_TOLERANCE 0.18f /**< Bezier flatness tolerance (px²). Coarser = fewer edges (AA hides facets). */
#define VEC_ARC_TOLERANCE                                                                                              \
    0.10f                        /**< Arc chord-error tolerance (px). Geometry build is ~1ms, so keep this fine:       \
                                    the rasterize cost is clip-area bound, not edge-count bound, and a coarse          \
                                    value visibly facets small curves (0.25f measures ~13% faster on a stroked         \
                                    ring and shifts its edge by a visible quarter pixel — not worth it). */
#define VEC_JOIN_TOLERANCE 0.05f /**< How far (px) a merged stroke corner may stray from the join it replaces. */
/* Round caps and round joins are polygons, and their vertex counts are what keeps a stroke inside
 * ERUI_VECTOR_MAX_EDGES: a path with many round joins spends its edge budget here first. */
#define VEC_DISC_SEGMENTS_PER_PX 1.5f /**< Cap-disc segments per pixel of radius, before the clamps. */
#define VEC_DISC_SEGMENTS_MIN 8       /**< Below this a small cap reads as a visible polygon. */
#define VEC_DISC_SEGMENTS_MAX 28      /**< Above this a large cap costs edges without looking rounder. */
#define VEC_JOIN_ARC_STEP 0.4f        /**< Round-join fan step in radians (~23 deg); one triangle per step. */
#define VEC_CLOSE_EPSILON 0.05f /**< Under this gap, a CLOSE that misses its start point counts as landing on it. */

/* Coverage at or above which a pixel quantizes to alpha 255 ((uint32)(c * 255 + 0.5f) == 255) — i.e.
 * where a row stops being an AA fringe and becomes solid paint. */
#define VEC_FULL_COVERAGE (254.5f / 255.0f)
/* A fully-covered run at least this wide leaves the row blend and goes out as its own er_blit_fill: a
 * fill carries no source pixels, and 64 px is where the DMA2D backend hands a span to its
 * register-to-memory transfer instead of the CPU (its min_dma_pixels default). Below that the two extra
 * backend calls the split costs are worth more than the source reads they save. */
#define VEC_ROW_FILL_MIN 64

#define VEC_FLATTEN_DEPTH_MAX 12        /**< Bezier subdivisions before a curve is taken as flat. */
#define VEC_ARC_MIN_RADIUS 0.5f         /**< Below this radius an arc is sampled at a half-turn step. */
#define VEC_ARC_FALLBACK_STEP 0.2f      /**< Arc step in radians when the chord formula gives none. */
#define VEC_DEGENERATE_EPSILON 1e-6f    /**< Lengths, crosses and radii below this have no direction. */
#define VEC_UNBOUNDED 1e30f             /**< Starting value for a min/max bounds scan. */
#define VEC_NO_ALPHA 0x100U             /**< Not an 8-bit alpha, so the premultiply cache always misses first. */
#define VEC_JOIN_SLIDE_LIMIT 0.25f      /**< Share of the shorter segment a merged corner may slide along. */
#define VEC_CAP_REACH 1.5f              /**< Caps and bevel joins stay within this many half-widths. */
#define VEC_DEFAULT_MITER_LIMIT 4.0f    /**< SVG's default stroke-miterlimit. */
#define VEC_FULL_TURN_EPSILON 1e-4f     /**< A sweep this close to a turn draws a whole circle. */
#define VEC_ARC_START_EPSILON 0.01f     /**< How far a MOVE may sit from the arc's start and still lead into it. */
#define VEC_QUAD_TO_CUBIC (2.0f / 3.0f) /**< Control-point weight when raising a quadratic to a cubic. */

/* Op-tape layout: floats an op occupies, opcode included, and the coordinates each op carries. */
#define VEC_MOVE_OP_SIZE 3
#define VEC_ARC_OP_SIZE 7
#define VEC_POINT_ARGS 2
#define VEC_QUAD_ARGS 4
#define VEC_CURVE_ARGS 6

/* Route a shape that IS just a circular arc at the shared analytic core (rendering/arc.c) instead of
 * flattening + stroking it — so an <Arc> or <Circle> in an <Svg> rasterizes through exactly the same
 * code (and anti-aliases identically to) a native ER_NODE_ARC widget, at a fraction of the cost. Only
 * shapes that map EXACTLY are routed (see arc_run_match / arc_paint_ok); everything else keeps the
 * general path, so this can never change a shape it does not fully describe. Set to 0 to restore the
 * pre-#87 all-tessellated behaviour. */
#ifndef ERUI_VECTOR_ANALYTIC_ARC
#define ERUI_VECTOR_ANALYTIC_ARC 1
#endif

/* ERUI_VECTOR_WARN_ONCE lives in vector.h (shared with vector_store.c); the knob behind it is
 * ERUI_DIAGNOSTICS in er_diagnostics.h. */

/*----------------------------------------------------------------------------------------------------------------------
 - Working state (static, reused per render)
 ---------------------------------------------------------------------------------------------------------------------*/

/* One rasterizer edge: ERVecEdge, defined in vector_cache.h — the edge cache (vector_cache.c) stores
 * arrays of them, and replay hands them straight back to rasterize(). */

typedef struct
{
    int start; /**< Index into s_point_x/s_point_y of the first vertex. */
    int count; /**< Vertex count. */
    int closed;
} VecSub;

static float s_point_x[ERUI_VECTOR_MAX_POINTS];
static float s_point_y[ERUI_VECTOR_MAX_POINTS];
static int s_point_count;
static VecSub s_subpaths[ERUI_VECTOR_MAX_SUBPATHS];
static int s_subpath_count;

static ERVecEdge s_edges[ERUI_VECTOR_MAX_EDGES];
static int s_edge_count;
static bool s_edge_truncated; /**< The current build dropped an edge on the pool cap (recording aborts). */
static float s_edge_y_top, s_edge_y_bottom; /**< Clip rows for the current pass; edge_add drops edges outside them. */

/* Edge ordering for the active-edge table: a counting sort writes indices into s_order, and s_buckets ends
 * up holding each bucket's END offset (see rasterize). Indices, not edges, so the sort moves 2 bytes
 * per edge instead of 20 — this is hot per-frame internal RAM. */
#if ERUI_VECTOR_MAX_EDGES <= UINT16_MAX
typedef uint16_t VecIdx;
#else
typedef uint32_t VecIdx;
#endif
static VecIdx s_order[ERUI_VECTOR_MAX_EDGES];
static VecIdx s_buckets[ERUI_VECTOR_SORT_BUCKETS];

static float s_cover[ERUI_VECTOR_MAX_ROW]; /**< Per-pixel coverage accumulator for the current row. */
static int s_row_low, s_row_high;          /**< Touched x-range (clip-local, [lo,hi)) in s_cover for the current row. */

/* One row of PREMULTIPLIED ARGB8888 — the AA path's staging buffer, handed to the backend by emit_row
 * as a single er_blit_blend per covered stretch. Sized like the coverage row (one entry per clip
 * column); the solid and gradient fills share it, so it costs one buffer either way. */
static uint32_t s_row[ERUI_VECTOR_MAX_ROW];

/* Crossing list reused per sub-scanline. */
static float s_cross_x[ERUI_VECTOR_MAX_EDGES];
static int s_cross_direction[ERUI_VECTOR_MAX_EDGES];

#if ERUI_VECTOR_EDGE_CACHE
/* Edge-cache recording state. Non-NULL while er_vector_render_slot is recording a render into a cache
 * entry (vector_cache.c owns the pool). While recording, edge builds run UNCLIPPED (the cache must
 * hold the node's whole geometry, not just this clip's slice) and the per-shape clip reject is off —
 * a one-render cost paid only for a tape that already proved static (see er_vector_cache_begin). */
static ERVecCache* s_recording;
static bool s_recording_failed; /**< The recording overflowed/truncated; entry will be discarded. */
#define VEC_RECORDING() (s_recording != 0 && !s_recording_failed)
#else
#define VEC_RECORDING() false
#endif

/* The per-node op-tape/paint storage pool (er_vector_store/free/reset/slot_ops/slot_paints + s_slots) lives
 * in vector_store.c — a separate translation unit so a target can place that cold pool in PSRAM (via the
 * engine's linker fragment) while this file's hot per-pixel scratch above stays in fast internal RAM. */

/*----------------------------------------------------------------------------------------------------------------------
 - Geometry building
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Appends a vertex to the current subpath (last entry of s_subpaths). */
static void pt_add(float x, float y)
{
    if (s_point_count >= ERUI_VECTOR_MAX_POINTS)
    {
        ERUI_VECTOR_WARN_ONCE("ERUI_VECTOR_MAX_POINTS", ERUI_VECTOR_MAX_POINTS);
        return;
    }
    if (s_subpath_count == 0)
        return;
    /* Drop exact duplicates (zero-length segments add nothing and bloat the edge list). */
    VecSub* subpath = &s_subpaths[s_subpath_count - 1];
    if (subpath->count > 0)
    {
        const int last = subpath->start + subpath->count - 1;
        if (s_point_x[last] == x && s_point_y[last] == y)
            return;
    }
    s_point_x[s_point_count] = x;
    s_point_y[s_point_count] = y;
    s_point_count++;
    subpath->count++;
}

/** @brief Starts a new subpath at (x,y). */
static void sub_begin(float x, float y)
{
    if (s_subpath_count >= ERUI_VECTOR_MAX_SUBPATHS)
    {
        ERUI_VECTOR_WARN_ONCE("ERUI_VECTOR_MAX_SUBPATHS", ERUI_VECTOR_MAX_SUBPATHS);
        return;
    }
    s_subpaths[s_subpath_count].start = s_point_count;
    s_subpaths[s_subpath_count].count = 0;
    s_subpaths[s_subpath_count].closed = 0;
    s_subpath_count++;
    pt_add(x, y);
}

/** @brief Recursively flattens a cubic bezier into line vertices. */
static void flatten_cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int depth)
{
    if (depth > VEC_FLATTEN_DEPTH_MAX)
    {
        pt_add(x3, y3);
        return;
    }
    const float chord_x = x3 - x0;
    const float chord_y = y3 - y0;
    const float distance1 = fabsf((x1 - x3) * chord_y - (y1 - y3) * chord_x);
    const float distance2 = fabsf((x2 - x3) * chord_y - (y2 - y3) * chord_x);
    if ((distance1 + distance2) * (distance1 + distance2)
        < VEC_FLATNESS_TOLERANCE * (chord_x * chord_x + chord_y * chord_y))
    {
        pt_add(x3, y3);
        return;
    }
    const float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    const float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    const float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
    const float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    const float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
    const float x0123 = (x012 + x123) * 0.5f, y0123 = (y012 + y123) * 0.5f;
    flatten_cubic(x0, y0, x01, y01, x012, y012, x0123, y0123, depth + 1);
    flatten_cubic(x0123, y0123, x123, y123, x23, y23, x3, y3, depth + 1);
}

/**
 * @brief An arc's sweep end_angle - start_angle as it is drawn: forward for clockwise, back for counter-clockwise, and
 * an opposite sweep wrapped by a turn.
 *
 * A turn or more in the drawing direction is a full turn, as a canvas arc is, and so are two finite ends too far
 * apart to subtract. The sweep is app geometry, and past a certain size adding 2π no longer changes it, so the
 * wrap is a fmodf rather than a loop that would never end.
 *
 * @return The sweep, at most a turn either way; NaN for a NaN or infinite end, which draws nothing.
 */
static float arc_sweep(float start_angle, float end_angle, bool counter_clockwise)
{
    const float turn = 2.0f * ER_PI;
    if (!(fabsf(start_angle) < INFINITY && fabsf(end_angle) < INFINITY))
        return NAN;
    float sweep = end_angle - start_angle;
    if (counter_clockwise ? sweep <= -turn : sweep >= turn)
        return counter_clockwise ? -turn : turn;
    /* The opposite way round, ends too far apart to subtract are each brought within a turn first. */
    if (!(fabsf(sweep) < INFINITY))
        sweep = fmodf(fmodf(end_angle, turn) - fmodf(start_angle, turn), turn);
    if (counter_clockwise ? sweep > 0.0f : sweep < 0.0f)
    {
        sweep = fmodf(sweep, turn);
        if (counter_clockwise ? sweep > 0.0f : sweep < 0.0f)
            sweep += counter_clockwise ? -turn : turn;
    }
    return sweep;
}

/** @brief Appends a circular arc, sampled so the chord error stays sub-pixel. */
static void
append_arc(float center_x, float center_y, float radius, float start_angle, float end_angle, int counter_clockwise)
{
    if (radius < 0.0f)
        radius = -radius;
    const float sweep = arc_sweep(start_angle, end_angle, counter_clockwise != 0);
    if (sweep != sweep)
        return; /* nothing to draw */
    /* Step angle so the chord deviation radius*(1-cos(step/2)) stays under VEC_ARC_TOLERANCE px. */
    float step = (radius > VEC_ARC_MIN_RADIUS) ? 2.0f * acosf(1.0f - VEC_ARC_TOLERANCE / radius) : ER_PI;
    if (step <= 0.0f || step != step) /* NaN/0 guard */
        step = VEC_ARC_FALLBACK_STEP;
    int step_count = (int)ceilf(fabsf(sweep) / step);
    if (step_count < 1)
        step_count = 1;
    for (int step_index = 0; step_index <= step_count; step_index++)
    {
        const float angle = start_angle + sweep * ((float)step_index / (float)step_count);
        pt_add(center_x + radius * cosf(angle), center_y + radius * sinf(angle));
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Edge list + rasterization
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief The scanline row, counted from @p row_min, that an edge starting at @p y0 is activated on: 0 for one starting
 *        above the rows, -1 for one starting at or below @p row_max (or at NaN), which never crosses a row.
 *
 * Compared in float before the int cast: an edge from app geometry can start past the int range.
 */
static inline int edge_start_row(float y0, int row_min, int row_max)
{
    if (!(y0 < (float)row_max))
        return -1;
    return y0 > (float)row_min ? (int)floorf(y0) - row_min : 0;
}

/** @brief Adds a non-horizontal edge to the rasterizer edge list (normalized so y0 <= y1). */
static void edge_add(float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return;

    if ((y0 <= s_edge_y_top && y1 <= s_edge_y_top) || (y0 >= s_edge_y_bottom && y1 >= s_edge_y_bottom))
        return;
    if (s_edge_count >= ERUI_VECTOR_MAX_EDGES)
    {
        ERUI_VECTOR_WARN_ONCE("ERUI_VECTOR_MAX_EDGES", ERUI_VECTOR_MAX_EDGES);
        s_edge_truncated = true;
        return;
    }
    ERVecEdge* edge = &s_edges[s_edge_count++];
    if (y0 < y1)
    {
        edge->x0 = x0;
        edge->y0 = y0;
        edge->x1 = x1;
        edge->y1 = y1;
        edge->direction = 1;
    }
    else
    {
        edge->x0 = x1;
        edge->y0 = y1;
        edge->x1 = x0;
        edge->y1 = y0;
        edge->direction = -1;
    }
}

/** @brief Adds coverage for a covered horizontal span [span_start,span_end] (screen px) at the given weight. */
static void cover_span(float span_start, float span_end, float weight, int clip_x0, int width)
{
    /* Reject non-finite spans: NaN/inf coordinates (from a malformed path) survive the clamps below and
     * turn (int)floorf(...) into a wild index that writes s_cover[] out of bounds. Defense in depth. */
    if (!isfinite(span_start) || !isfinite(span_end))
        return;
    span_start -= (float)clip_x0;
    span_end -= (float)clip_x0;
    if (span_start < 0.0f)
        span_start = 0.0f;
    if (span_end > (float)width)
        span_end = (float)width;
    if (span_end <= span_start)
        return;
    int cell_start = (int)floorf(span_start);
    int cell_end = (int)floorf(span_end);
    int high; /* highest cell index written, for the touched-range tracker */
    if (cell_start == cell_end)
    {
        s_cover[cell_start] += weight * (span_end - span_start);
        high = cell_start;
    }
    else
    {
        s_cover[cell_start] += weight * ((float)(cell_start + 1) - span_start);
        for (int cell = cell_start + 1; cell < cell_end; cell++)
            s_cover[cell] += weight;
        if (cell_end < width)
        {
            s_cover[cell_end] += weight * (span_end - (float)cell_end);
            high = cell_end;
        }
        else
        {
            high = cell_end - 1;
        }
    }
    /* Record the span so the row's clear + emit touch only covered pixels, not the whole clip width. */
    if (cell_start < s_row_low)
        s_row_low = cell_start;
    if (high + 1 > s_row_high)
        s_row_high = high + 1;
}

/** @brief Returns true when winding @p winding counts as "inside" under the given fill rule. */
static int rule_inside(int winding, int even_odd)
{
    return even_odd ? (winding & 1) : (winding != 0);
}

#if ERUI_GRADIENT
/* Vector gradient fill. Reuses gradient.c's always-compiled colour helpers, forward-declared here
 * to keep this TU decoupled from the node internals that gradient.h would pull in. */
uint32_t er_gradient_eval_stops(const ERGradientStop* stops, int count, float position);
uint32_t er_gradient_premul(uint32_t straight_argb);

/* Precomputed colour ramp: ERUI_VECTOR_GRAD_LUT premultiplied-ARGB entries sampled across t∈[0,1), rebuilt
 * once per gradient shape (build_grad_lut). The per-pixel sampler then indexes this instead of re-running the
 * stop search/interpolation (er_gradient_eval_stops) every pixel — the bulk of the conic dial's drag cost.
 * 256 matches 8-bit colour resolution; a RAM-tight board may lower it (a 1 KB internal buffer at 256) at the
 * cost of coarser colour steps. Tunable like the other vector pools (CMake cache var; see engine/README). */
#ifndef ERUI_VECTOR_GRAD_LUT
#define ERUI_VECTOR_GRAD_LUT 256
#endif
static uint32_t s_gradient_lut[ERUI_VECTOR_GRAD_LUT];

/** @brief Builds the premultiplied colour LUT for a gradient (one er_gradient_eval_stops per entry). */
static void build_grad_lut(const ERVectorGradient* gradient)
{
    for (int entry_index = 0; entry_index < ERUI_VECTOR_GRAD_LUT; entry_index++)
    {
        const float position = ((float)entry_index + 0.5f) / (float)ERUI_VECTOR_GRAD_LUT;
        s_gradient_lut[entry_index] =
            er_gradient_premul(er_gradient_eval_stops(gradient->stops, gradient->stop_count, position));
    }
}

/** @brief Scales a premultiplied-ARGB pixel by an 8-bit coverage (all four channels), rounding to nearest. */
static inline uint32_t vgrad_scale(uint32_t pixel, uint32_t coverage)
{
    const uint32_t alpha = (((pixel >> 24) & 0xFFU) * coverage + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
    const uint32_t red = (((pixel >> 16) & 0xFFU) * coverage + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
    const uint32_t green = (((pixel >> 8) & 0xFFU) * coverage + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
    const uint32_t blue = ((pixel & 0xFFU) * coverage + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

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
static float vgrad_t(const ERVectorGradient* gradient, float pixel_x, float pixel_y)
{
#if ERUI_GRADIENT_RADIAL
    if (gradient->type == ER_GRADIENT_RADIAL)
    {
        const float offset_x = pixel_x - gradient->ax, offset_y = pixel_y - gradient->ay;
        return (gradient->r > VEC_DEGENERATE_EPSILON) ? sqrtf(offset_x * offset_x + offset_y * offset_y) / gradient->r
                                                      : 0.0f;
    }
#endif
#if ERUI_GRADIENT_CONIC
    if (gradient->type == ER_GRADIENT_CONIC)
    {
        /* atan2(dx, -dy): 0 at the top, increasing clockwise. Subtract the start angle, wrap to [0,1).
         * Uses the polynomial atan2 approximation — this runs per covered pixel on the drag hot path. */
        float turn_fraction =
            (er_fast_atan2(pixel_x - gradient->ax, -(pixel_y - gradient->ay)) - gradient->r) / (2.0f * ER_PI);
        turn_fraction -= floorf(turn_fraction);
        return turn_fraction;
    }
#endif
    {
        const float axis_x = gradient->bx - gradient->ax, axis_y = gradient->by - gradient->ay;
        const float axis_length_squared = axis_x * axis_x + axis_y * axis_y;
        if (axis_length_squared < VEC_DEGENERATE_EPSILON)
            return 0.0f;
        return ((pixel_x - gradient->ax) * axis_x + (pixel_y - gradient->ay) * axis_y) / axis_length_squared;
    }
}

/**
 * @brief Resolves a paint's 1-based gradient index into a framebuffer-space gradient (or NULL).
 *        Copies the table entry into @p resolved, offsets its op-tape (node-local) geometry by the node origin
 *        (origin_x, origin_y) to match the flattened path, and returns @p resolved — or NULL if the index is out
 *        of range, the type is unsupported in this build, or there are < 2 stops. Used for both fill and stroke.
 */
static const ERVectorGradient* resolve_grad(int gradient_number,
                                            const ERVectorGradient* gradients,
                                            int gradient_count,
                                            int origin_x,
                                            int origin_y,
                                            ERVectorGradient* resolved)
{
    if (gradient_number <= 0 || gradient_number > gradient_count || !gradients)
        return NULL;
    *resolved = gradients[gradient_number - 1];
    if (!vgrad_supported(resolved->type) || resolved->stop_count < 2)
        return NULL;
    resolved->ax += (float)origin_x; /* r is a length and is NOT offset */
    resolved->ay += (float)origin_y;
    resolved->bx += (float)origin_x;
    resolved->by += (float)origin_y;
    return resolved;
}
#endif /* ERUI_GRADIENT */

/** @brief Hands @p pixel_count staged pixels of s_row, starting at clip-local @p x0, to the backend as one blend. */
static void flush_row(int x0, int pixel_count, int row_y, int clip_x0)
{
    er_blit_blend(
        s_row, (int)(sizeof(uint32_t) * (size_t)pixel_count), ER_ALPHA_OPAQUE, clip_x0 + x0, row_y, pixel_count, 1);
}

/**
 * @brief Blends the accumulated coverage row into the target, one backend call per covered stretch.
 *
 * Scans only [low,high) — the x-range cover_span() actually touched this row — and zeroes each
 * consumed cell so the next row starts clean without a full-width memset. For a thin stroke in a
 * wide clip this turns the per-row floor from O(clip width) into O(covered width).
 *
 * Covered pixels are staged into s_row as premultiplied ARGB and flushed through flush_row, instead
 * of going out as one er_blit_fill per equal-alpha run. An AA fringe carries a different alpha every
 * pixel, so that split cost one backend call — one clip walk, one dispatch, and on the DMA2D one
 * transfer fence — per PIXEL, and a row of a stroked ring cost a dozen or more. A stretch ends at the
 * first pixel whose coverage quantizes to alpha 0, so the pixels handed to the backend are exactly
 * the ones the run split wrote: a gap between subpaths (the two bands of a ring row) still costs
 * nothing, and no uncovered pixel is ever pushed through the blend.
 *
 * When @p gradient is non-NULL the fill colour is sampled per pixel from the gradient instead of the
 * constant @p color; that path stages uncovered pixels as alpha 0 (a no-op at the backend) and
 * flushes the whole range in one call.
 */
static void emit_row(uint32_t color, const ERVectorGradient* gradient, int row_y, int clip_x0, int low, int high)
{
#if ERUI_GRADIENT
    if (gradient)
    {
        const int pixel_count = high - low;
        for (int offset = 0; offset < pixel_count; offset++)
        {
            float coverage = s_cover[low + offset];
            s_cover[low + offset] = 0.0f; /* consume so the cell is clean for the next row */
            if (coverage <= 0.0f)
            {
                s_row[offset] = 0u; /* uncovered: alpha 0 leaves the destination untouched */
                continue;
            }
            if (coverage > 1.0f)
                coverage = 1.0f;
            /* position -> LUT index (clamped; eval clamps to the endpoint colours, so an out-of-range position pins to
             * an end). The LUT entry is premultiplied, so scaling all four channels by coverage finishes the pixel — no
             * per-pixel stop interpolation or premultiply. */
            const float position = vgrad_t(gradient, (float)(clip_x0 + low + offset) + 0.5f, (float)row_y + 0.5f);
            const float scaled = position * (float)ERUI_VECTOR_GRAD_LUT;
            const int lut_index = !(scaled > 0.0f)                        ? 0
                                  : scaled >= (float)ERUI_VECTOR_GRAD_LUT ? ERUI_VECTOR_GRAD_LUT - 1
                                                                          : (int)scaled;
            s_row[offset] = vgrad_scale(s_gradient_lut[lut_index], (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f));
        }
        flush_row(low, pixel_count, row_y, clip_x0);
        return;
    }
#else
    (void)gradient;
#endif
    const uint32_t paint_alpha = (color >> 24) & 0xFFU;
    const uint32_t color_red = (color >> 16) & 0xFFU, color_green = (color >> 8) & 0xFFU, color_blue = color & 0xFFU;
    /* A fully-covered pixel keeps the paint's own alpha, so its premultiplied value is fixed. */
    const uint32_t full_pixel = (paint_alpha << 24)
                                | (((color_red * paint_alpha + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX) << 16)
                                | (((color_green * paint_alpha + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX) << 8)
                                | ((color_blue * paint_alpha + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX);
    /* One-entry premultiply cache: an AA fringe holds its alpha for a few pixels at a time. 0x100 is an
     * impossible alpha, so the first pixel of the row always misses. */
    uint32_t last_alpha = VEC_NO_ALPHA, last_pixel = 0U;
    int stretch_start = low; /* clip-local x of s_row[0] */
    int staged_count = 0;    /* pixels staged in s_row, not yet flushed */
    int column = low;
    while (column < high)
    {
        const float cell_coverage = s_cover[column];
        if (cell_coverage <= 0.0f)
        {
            /* Uncovered: the cells are already zero, so there is nothing to consume — but they end the
             * stretch, so the gap between the two bands of a ring row is never pushed through a blend.
             * Skipped as a run: for a thin stroke in a wide clip the gaps ARE the row. */
            if (staged_count > 0)
            {
                flush_row(stretch_start, staged_count, row_y, clip_x0);
                staged_count = 0;
            }
            do
            {
                column++;
            } while (column < high && s_cover[column] <= 0.0f);
            stretch_start = column;
            continue;
        }
        if (cell_coverage >= VEC_FULL_COVERAGE)
        {
            /* A fully-covered run is the paint colour at its own alpha from end to end, so it needs
             * none of the per-pixel coverage arithmetic — and past VEC_ROW_FILL_MIN no source row
             * either: it goes out as the one span shape a backend can hand to its blitter, which is
             * how a filled shape's interior stays a single fill per row (and DMA-able) as before. */
            int run = 1;
            while (column + run < high && s_cover[column + run] >= VEC_FULL_COVERAGE)
                run++;
            /* Consume the run so the cells are clean for the next row. memset because these runs are
             * long (a filled shape's interior is one of them) and 0.0f is all-zero bits; the per-cell
             * store below measures ~10% slower on a large fill. */
            memset(&s_cover[column], 0, sizeof(float) * (size_t)run);
            if (run >= VEC_ROW_FILL_MIN)
            {
                if (staged_count > 0)
                {
                    flush_row(stretch_start, staged_count, row_y, clip_x0);
                    staged_count = 0;
                }
                er_blit_fill(color, clip_x0 + column, row_y, run, 1);
                column += run;
                stretch_start = column;
                continue;
            }
            for (int run_index = 0; run_index < run; run_index++)
                s_row[staged_count++] = full_pixel;
            column += run;
            continue;
        }
        float coverage = cell_coverage;
        s_cover[column] = 0.0f; /* consume so the cell is clean for the next row */
        column++;
        if (coverage > 1.0f)
            coverage = 1.0f;
        const uint32_t alpha =
            (paint_alpha * (uint32_t)(coverage * ER_CHANNEL_MAX_FLOAT + 0.5f) + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX;
        if (alpha == 0U)
        {
            /* Ink too faint to show: nothing to blend, and — like an uncovered cell — it ends the
             * stretch, so the backend is handed exactly the pixels the per-run fills used to write. */
            if (staged_count > 0)
            {
                flush_row(stretch_start, staged_count, row_y, clip_x0);
                staged_count = 0;
            }
            stretch_start = column;
            continue;
        }
        if (alpha != last_alpha)
        {
            last_alpha = alpha;
            /* +127 rounds, which is what the backends' own div255 does when they premultiply a
             * straight-alpha fill — so an AA pixel keeps the exact value it had before the row blend
             * took over from er_blit_fill. */
            last_pixel = (alpha << 24) | (((color_red * alpha + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX) << 16)
                         | (((color_green * alpha + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX) << 8)
                         | ((color_blue * alpha + ER_CHANNEL_ROUND) / ER_CHANNEL_MAX);
        }
        s_row[staged_count++] = last_pixel;
    }
    if (staged_count > 0)
        flush_row(stretch_start, staged_count, row_y, clip_x0);
}

/* Indices of edges crossing the current scanline (the active-edge table). */
static int s_active[ERUI_VECTOR_MAX_EDGES];

/**
 * @brief Rasterizes an edge list into the clip box with anti-aliasing.
 *
 * @p edges is normally this file's own scratch (s_edges) just built for the pass, but a cache replay
 * hands in a recorded list instead — the pointer indirection is what lets replay skip the geometry
 * build entirely. @p edge_count must not exceed ERUI_VECTOR_MAX_EDGES (the sort/active tables are sized
 * to it); every producer — the build path and the cache recorder — enforces that cap.
 */
static void rasterize(const ERVecEdge* edges,
                      int edge_count,
                      uint32_t color,
                      const ERVectorGradient* gradient,
                      int even_odd,
                      int clip_x0,
                      int clip_y0,
                      int clip_x1,
                      int clip_y1)
{
    /* Bail on a fully-transparent solid colour — unless a gradient supplies the fill colour instead. */
    if (edge_count == 0 || (!gradient && ((color >> 24) & 0xFFU) == 0U))
        return;
    const int width = clip_x1 - clip_x0;
    if (width <= 0)
        return;
    if (width > ERUI_VECTOR_MAX_ROW)
    {
        ERUI_VECTOR_WARN_ONCE("ERUI_VECTOR_MAX_ROW", ERUI_VECTOR_MAX_ROW);
        return;
    }
#if ERUI_GRADIENT
    /* Build the colour LUT once per gradient shape — the per-pixel sampler (emit_row) then indexes it
     * instead of interpolating the stops every pixel. */
    if (gradient)
        build_grad_lut(gradient);
#endif

    float top_y = VEC_UNBOUNDED, bottom_y = -VEC_UNBOUNDED;
    for (int edge_index = 0; edge_index < edge_count; edge_index++)
    {
        if (edges[edge_index].y0 < top_y)
            top_y = edges[edge_index].y0;
        if (edges[edge_index].y1 > bottom_y)
            bottom_y = edges[edge_index].y1;
    }
    /* Clamped to the clip in float first: an edge from app geometry can run past the int range, where the cast is
     * undefined. */
    int row_min = top_y > (float)clip_y0 ? (top_y < (float)clip_y1 ? (int)floorf(top_y) : clip_y1) : clip_y0;
    int row_max = bottom_y < (float)clip_y1 ? (bottom_y > (float)clip_y0 ? (int)ceilf(bottom_y) : clip_y0) : clip_y1;
    if (row_max <= row_min)
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
    const int row_count = row_max - row_min;
    int shift = 0;
    while ((row_count >> shift) >= ERUI_VECTOR_SORT_BUCKETS)
        shift++;
    const int bucket_count = ((row_count - 1) >> shift) + 1;
    memset(s_buckets, 0, sizeof(VecIdx) * (size_t)bucket_count);
    for (int edge_index = 0; edge_index < edge_count; edge_index++)
    {
        const int row = edge_start_row(edges[edge_index].y0, row_min, row_max);
        if (row < 0)
            continue; /* starts below the last row scanned: it can never cross one */
        s_buckets[row >> shift]++;
    }
    int total = 0;
    for (int bucket = 0; bucket < bucket_count; bucket++)
    {
        const int bucket_size = (int)s_buckets[bucket];
        s_buckets[bucket] = (VecIdx)total; /* bucket start; the placement pass advances it to the bucket END */
        total += bucket_size;
    }
    if (total == 0)
        return;
    for (int edge_index = 0; edge_index < edge_count; edge_index++)
    {
        const int row = edge_start_row(edges[edge_index].y0, row_min, row_max);
        if (row < 0)
            continue;
        s_order[s_buckets[row >> shift]++] = (VecIdx)edge_index;
    }

    const float subsample_weight = 1.0f / (float)VEC_SUBSAMPLES;
    int next = 0;         /* next edge (in bucket order) not yet activated */
    int active_count = 0; /* count in s_active */

    for (int row_y = row_min; row_y < row_max; row_y++)
    {
        /* Activate this row's bucket (and any before it); retire edges that ended above the row. */
        const int activate_end = (int)s_buckets[(row_y - row_min) >> shift];
        while (next < activate_end)
            s_active[active_count++] = (int)s_order[next++];
        for (int active_index = 0; active_index < active_count;)
        {
            if (edges[s_active[active_index]].y1 <= (float)row_y)
                s_active[active_index] = s_active[--active_count];
            else
                active_index++;
        }

        /* s_cover is left zeroed by the previous row's emit_row (which clears every cell it consumes),
         * so no full-width memset is needed; track the x-range this row actually touches instead. */
        s_row_low = width;
        s_row_high = 0;
        for (int subsample = 0; subsample < VEC_SUBSAMPLES; subsample++)
        {
            const float sample_y = (float)row_y + ((float)subsample + 0.5f) * subsample_weight;
            /* Collect crossings of this sub-scanline from the active set only. */
            int cross_count = 0;
            for (int active_index = 0; active_index < active_count; active_index++)
            {
                const ERVecEdge* edge = &edges[s_active[active_index]];
                if (sample_y < edge->y0 || sample_y >= edge->y1)
                    continue;
                const float fraction = (sample_y - edge->y0) / (edge->y1 - edge->y0);
                s_cross_x[cross_count] = edge->x0 + (edge->x1 - edge->x0) * fraction;
                s_cross_direction[cross_count] = edge->direction;
                cross_count++;
            }
            if (cross_count < 2)
                continue;
            /* Insertion sort crossings by x (m is small in practice). */
            for (int cross_index = 1; cross_index < cross_count; cross_index++)
            {
                const float key_x = s_cross_x[cross_index];
                const int key_direction = s_cross_direction[cross_index];
                int insert_index = cross_index - 1;
                while (insert_index >= 0 && s_cross_x[insert_index] > key_x)
                {
                    s_cross_x[insert_index + 1] = s_cross_x[insert_index];
                    s_cross_direction[insert_index + 1] = s_cross_direction[insert_index];
                    insert_index--;
                }
                s_cross_x[insert_index + 1] = key_x;
                s_cross_direction[insert_index + 1] = key_direction;
            }
            /* Walk crossings, accumulating winding; emit covered spans. */
            int winding = 0;
            for (int cross_index = 0; cross_index < cross_count - 1; cross_index++)
            {
                winding += s_cross_direction[cross_index];
                if (rule_inside(winding, even_odd))
                    cover_span(s_cross_x[cross_index], s_cross_x[cross_index + 1], subsample_weight, clip_x0, width);
            }
        }
        if (s_row_high > s_row_low)
            emit_row(color, gradient, row_y, clip_x0, s_row_low, s_row_high);
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
static bool record_pass(uint8_t kind, int paint_index)
{
    ERVecCache* entry = s_recording;
    if (s_edge_count == 0)
        return true; /* nothing painted, nothing to replay */
    if (entry->pass_count >= ERUI_VECTOR_CACHE_PASSES || entry->edge_count + s_edge_count > ERUI_VECTOR_CACHE_EDGES)
    {
        ERUI_WARN_ONCE("embedded-react vector: edge cache entry full (ERUI_VECTOR_CACHE_EDGES %d / "
                       "ERUI_VECTOR_CACHE_PASSES %d) - node repaints uncached; raise them.\n",
                       (int)ERUI_VECTOR_CACHE_EDGES,
                       (int)ERUI_VECTOR_CACHE_PASSES);
        return false;
    }
    ERVecPass* pass = &entry->passes[entry->pass_count++];
    pass->kind = kind;
    pass->paint = (uint16_t)paint_index;
    pass->start = entry->edge_count;
    pass->count = s_edge_count;
    float bounds_x0 = VEC_UNBOUNDED, bounds_y0 = VEC_UNBOUNDED, bounds_x1 = -VEC_UNBOUNDED, bounds_y1 = -VEC_UNBOUNDED;
    for (int edge_index = 0; edge_index < s_edge_count; edge_index++)
    {
        const ERVecEdge* edge = &s_edges[edge_index];
        const float low_x = (edge->x0 < edge->x1) ? edge->x0 : edge->x1;
        const float high_x = (edge->x0 < edge->x1) ? edge->x1 : edge->x0;
        if (low_x < bounds_x0)
            bounds_x0 = low_x;
        if (high_x > bounds_x1)
            bounds_x1 = high_x;
        if (edge->y0 < bounds_y0)
            bounds_y0 = edge->y0; /* edges are normalized y0 <= y1 */
        if (edge->y1 > bounds_y1)
            bounds_y1 = edge->y1;
    }
    pass->bounds_x0 = bounds_x0;
    pass->bounds_y0 = bounds_y0;
    pass->bounds_x1 = bounds_x1;
    pass->bounds_y1 = bounds_y1;
    memcpy(entry->edges + entry->edge_count, s_edges, (size_t)s_edge_count * sizeof(ERVecEdge));
    entry->edge_count += s_edge_count;
    return true;
}
#endif /* ERUI_VECTOR_EDGE_CACHE */

/** @brief Builds closed fill edges from every subpath (into s_edges, honoring s_edge_y_top/s_edge_y_bottom). */
static void fill_build(void)
{
    s_edge_count = 0;
    s_edge_truncated = false;
    for (int subpath_index = 0; subpath_index < s_subpath_count; subpath_index++)
    {
        const VecSub* subpath = &s_subpaths[subpath_index];
        if (subpath->count < 2)
            continue;
        for (int point_index = 0; point_index < subpath->count - 1; point_index++)
            edge_add(s_point_x[subpath->start + point_index],
                     s_point_y[subpath->start + point_index],
                     s_point_x[subpath->start + point_index + 1],
                     s_point_y[subpath->start + point_index + 1]);
        /* Fill implicitly closes every subpath. */
        edge_add(s_point_x[subpath->start + subpath->count - 1],
                 s_point_y[subpath->start + subpath->count - 1],
                 s_point_x[subpath->start],
                 s_point_y[subpath->start]);
    }
}

/** @brief Builds closed edges from every subpath and rasterizes the fill (recording it when armed). */
static void fill_shape(uint32_t color,
                       const ERVectorGradient* gradient,
                       int even_odd,
                       int paint_index,
                       int clip_x0,
                       int clip_y0,
                       int clip_x1,
                       int clip_y1)
{
#if ERUI_VECTOR_EDGE_CACHE
    if (VEC_RECORDING())
    {
        /* Record mode: build the FULL edge list (no clip-row drop) so any later clip can replay it. */
        s_edge_y_top = -VEC_UNBOUNDED;
        s_edge_y_bottom = VEC_UNBOUNDED;
        fill_build();
        if (s_edge_truncated)
        {
            /* The unclipped build overflowed the edge pool; a clipped build may keep edges this one
             * dropped, so abandon the recording and fall through to the legacy build for THIS render. */
            s_recording_failed = true;
        }
        else
        {
            if (!record_pass(ER_VEC_PASS_FILL, paint_index))
                s_recording_failed = true;
            /* The built edges are complete either way — rasterize them directly. */
            rasterize(s_edges, s_edge_count, color, gradient, even_odd, clip_x0, clip_y0, clip_x1, clip_y1);
            return;
        }
    }
#else
    (void)paint_index;
#endif
    s_edge_y_top = (float)clip_y0;
    s_edge_y_bottom = (float)clip_y1;
    fill_build();
    rasterize(s_edges, s_edge_count, color, gradient, even_odd, clip_x0, clip_y0, clip_x1, clip_y1);
}

/** @brief Adds a filled convex quad (4 corners, CCW or CW) as edges. */
static void add_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    edge_add(x0, y0, x1, y1);
    edge_add(x1, y1, x2, y2);
    edge_add(x2, y2, x3, y3);
    edge_add(x3, y3, x0, y0);
}

/**
 * @brief Adds a filled disc (used for round caps) approximated by an n-gon.
 *
 * The perimeter is traversed CLOCKWISE (angle decreasing) so the disc's winding sign matches the segment
 * quads from add_quad(). A round cap disc overlaps the stroke body it sits on; under the nonzero rule
 * matching signs make the overlap add (stay filled) — the opposite winding would cancel it to zero and
 * punch a half-circle hole at each arc end.
 */
static void add_disc(float center_x, float center_y, float radius)
{
    if (!(radius > 0.0f))
        return;
    /* Capped before the cast: a stroke width from app geometry can run past the int range. */
    int segment_count = radius < (float)VEC_DISC_SEGMENTS_MAX / VEC_DISC_SEGMENTS_PER_PX
                            ? (int)(radius * VEC_DISC_SEGMENTS_PER_PX)
                            : VEC_DISC_SEGMENTS_MAX;
    if (segment_count < VEC_DISC_SEGMENTS_MIN)
        segment_count = VEC_DISC_SEGMENTS_MIN;
    if (segment_count > VEC_DISC_SEGMENTS_MAX)
        segment_count = VEC_DISC_SEGMENTS_MAX;
    float previous_x = center_x + radius, previous_y = center_y;
    for (int segment_index = 1; segment_index <= segment_count; segment_index++)
    {
        const float angle = -2.0f * ER_PI * (float)segment_index / (float)segment_count;
        const float next_x = center_x + radius * cosf(angle);
        const float next_y = center_y + radius * sinf(angle);
        edge_add(previous_x, previous_y, next_x, next_y);
        previous_x = next_x;
        previous_y = next_y;
    }
}

/** @brief Adds a triangle as edges (for bevel/miter joins). */
static void add_tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    edge_add(x0, y0, x1, y1);
    edge_add(x1, y1, x2, y2);
    edge_add(x2, y2, x0, y0);
}

/** @brief Unit direction + length of a segment; false when it is degenerate (nothing to stroke). */
static bool seg_dir(float x0, float y0, float x1, float y1, float* unit_x, float* unit_y, float* length)
{
    const float delta_x = x1 - x0, delta_y = y1 - y0;
    const float segment_length = sqrtf(delta_x * delta_x + delta_y * delta_y);
    if (segment_length < VEC_DEGENERATE_EPSILON)
        return false;
    *unit_x = delta_x / segment_length;
    *unit_y = delta_y / segment_length;
    *length = segment_length;
    return true;
}

/**
 * @brief Decides whether a vertex's join can be folded into its two segment quads instead of drawn.
 *
 * Two quads butted at a vertex leave the whole pie slice between them uncovered — the join's job is to
 * fill it, and dropping one notches the stroke by the slice's width (~half_width * turn angle), which is why a
 * near-collinear test alone cannot skip it without leaving a visible bite. Mitering BOTH sides of the
 * shared corner instead closes the slice exactly: the two quads then meet along the angle bisector,
 * with the corner pushed out to half_width/cos(t/2) — and when that is within VEC_JOIN_TOLERANCE of half_width, the
 * result is within a sub-pixel of the round/bevel join it replaces, whichever style is set. That makes it free to apply
 * on smooth flattened curves, where it drops the per-vertex join geometry (and its trig) entirely: an arc's every
 * vertex used to pay for a triangle fan covering a gap thinner than a pixel.
 *
 * @param[in]  in_x,in_y,in_length  Incoming segment direction (unit) and length.
 * @param[in]  out_x,out_y,out_length  Outgoing segment direction (unit) and length.
 * @param[in]  half_width  Stroke half-width.
 * @param[in]  merge_dot   Minimum dot product for the miter to stay within tolerance (see caller).
 * @param[out] offset_x,offset_y       Corner offset vector: the shared corners are vertex +/- (offset_x,offset_y).
 *
 * @return true when the join merged, in which case NO join geometry is needed at this vertex.
 */
static bool join_merge(float in_x,
                       float in_y,
                       float in_length,
                       float out_x,
                       float out_y,
                       float out_length,
                       float half_width,
                       float merge_dot,
                       float* offset_x,
                       float* offset_y)
{
    const float dot = in_x * out_x + in_y * out_y;
    if (dot <= 0.0f || dot < merge_dot)
        return false;
    const float cross = in_x * out_y - in_y * out_x;
    float slide = 0.0f; /* how far the corner slides ALONG the incoming segment; 0 when collinear */
    if (fabsf(cross) > VEC_DEGENERATE_EPSILON)
    {
        slide = half_width * (dot - 1.0f) / cross; /* = -/+ r * tan(t/2) */
        /* Never eat more than a quarter of either segment. Both ends of a segment slide, and on a
         * zigzag they slide OPPOSITE ways (the turn flips sign), so anything looser lets a quad pinch
         * shut at one corner and drop the sliver of coverage the join used to hold. Past the limit the
         * corner simply does not merge and the join is drawn as before. */
        const float slide_limit = VEC_JOIN_SLIDE_LIMIT * ((in_length < out_length) ? in_length : out_length);
        if (slide > slide_limit || slide < -slide_limit)
            return false;
    }
    *offset_x = -in_y * half_width + in_x * slide;
    *offset_y = in_x * half_width + in_y * slide;
    return true;
}

/**
 * @brief Adds the join geometry at a vertex whose corner could not be merged (a real corner).
 *
 * Round joins fan the outer gap with triangles back to the vertex, bevel uses one triangle, and miter
 * extends to the intersection of the outer offset lines (falling back to bevel past the miter limit).
 *
 * @param[in] vertex_x,vertex_y    Vertex position.
 * @param[in] in_x,in_y  Incoming segment direction (unit).
 * @param[in] out_x,out_y  Outgoing segment direction (unit).
 * @param[in] half_width  Stroke half-width.
 * @param[in] join        ER_VECTOR_JOIN_* style.
 * @param[in] miter       Miter limit (multiples of the half-width).
 */
static void add_join(float vertex_x,
                     float vertex_y,
                     float in_x,
                     float in_y,
                     float out_x,
                     float out_y,
                     float half_width,
                     int join,
                     float miter)
{
    /* Outer side normals + which side carries the gap. */
    const float in_normal_x = -in_y * half_width, in_normal_y = in_x * half_width;
    const float out_normal_x = -out_y * half_width, out_normal_y = out_x * half_width;
    const float cross = in_x * out_y - in_y * out_x;
    const float side = (cross < 0.0f) ? 1.0f : -1.0f; /* pick the outer offsets */
    const float outer_in_x = vertex_x + side * in_normal_x, outer_in_y = vertex_y + side * in_normal_y;
    const float outer_out_x = vertex_x + side * out_normal_x, outer_out_y = vertex_y + side * out_normal_y;

    if (join == ER_VECTOR_JOIN_ROUND)
    {
        /* Fan the outer gap with triangles back to the vertex — a true round join in a handful of
         * edges (one triangle for a gentle bend), not a full disc per vertex. */
        const float start_angle = atan2f(outer_in_y - vertex_y, outer_in_x - vertex_x);
        const float end_angle = atan2f(outer_out_y - vertex_y, outer_out_x - vertex_x);
        float turn = end_angle - start_angle;
        while (turn > ER_PI)
            turn -= 2.0f * ER_PI;
        while (turn < -ER_PI)
            turn += 2.0f * ER_PI;
        /* A NaN corner (app geometry) gives a NaN turn, which the int cast would leave undefined. */
        int steps = fabsf(turn) < INFINITY ? (int)ceilf(fabsf(turn) / VEC_JOIN_ARC_STEP) : 1;
        if (steps < 1)
            steps = 1;
        float fan_x = outer_in_x, fan_y = outer_in_y;
        for (int step = 1; step <= steps; step++)
        {
            const float angle = start_angle + turn * ((float)step / (float)steps);
            const float next_x = vertex_x + half_width * cosf(angle);
            const float next_y = vertex_y + half_width * sinf(angle);
            add_tri(vertex_x, vertex_y, fan_x, fan_y, next_x, next_y);
            fan_x = next_x;
            fan_y = next_y;
        }
        return;
    }
    if (join == ER_VECTOR_JOIN_MITER)
    {
        /* Miter point = intersection of the two outer offset lines. */
        const float denominator = cross;
        if (fabsf(denominator) > VEC_DEGENERATE_EPSILON)
        {
            const float distance =
                ((outer_out_x - outer_in_x) * out_y - (outer_out_y - outer_in_y) * out_x) / denominator;
            const float miter_x = outer_in_x + in_x * distance;
            const float miter_y = outer_in_y + in_y * distance;
            const float miter_offset_x = miter_x - vertex_x, miter_offset_y = miter_y - vertex_y;
            const float miter_length = sqrtf(miter_offset_x * miter_offset_x + miter_offset_y * miter_offset_y);
            if (miter_length <= miter * half_width)
            {
                add_tri(vertex_x, vertex_y, outer_in_x, outer_in_y, miter_x, miter_y);
                add_tri(vertex_x, vertex_y, miter_x, miter_y, outer_out_x, outer_out_y);
                return;
            }
        }
        /* Too sharp -> fall through to bevel. */
    }
    add_tri(vertex_x, vertex_y, outer_in_x, outer_in_y, outer_out_x, outer_out_y); /* bevel */
}

/** @brief Vertex a segment ends at. Only a closing segment (segment == vertex_count-1) wraps back to the start. */
static inline int seg_end_index(int segment, int vertex_count)
{
    return (segment + 1 == vertex_count) ? 0 : segment + 1;
}

/**
 * @brief Builds stroke outline geometry for one subpath and adds it to the edge list.
 *
 * Each segment becomes a quad of width @p stroke_width and open endpoints get a cap. At an interior vertex the
 * two quads either share a mitered corner (join_merge, the common case on a flattened curve) or butt
 * against each other and the gap between them is filled by join geometry (add_join). The pieces
 * overlap and are unioned by the nonzero fill rule, so no single clean outline polygon is needed.
 *
 * A closed subpath whose CLOSE does not land back on its start point gets one extra segment for that
 * closing edge, so the segment list wraps and its two ends are corners like any other.
 */
static void stroke_subpath(const VecSub* subpath, float stroke_width, int cap, int join, float miter)
{
    const int vertex_count = subpath->count;
    if (vertex_count < 1)
        return;
    const float half_width = stroke_width * 0.5f;
    const float* points_x = &s_point_x[subpath->start];
    const float* points_y = &s_point_y[subpath->start];

    if (vertex_count == 1)
    {
        if (cap == ER_VECTOR_CAP_ROUND)
            add_disc(points_x[0], points_y[0], half_width);
        return;
    }

    const int closed = subpath->closed;

    /* Merge a corner while the miter it produces stays within VEC_JOIN_TOLERANCE of the half-width:
     * |corner| = half_width / cos(t/2), so the bound is cos(t/2) >= half_width / (half_width + tol), and dot =
     * 2cos²(t/2) - 1. */
    const float cos_limit = half_width / (half_width + VEC_JOIN_TOLERANCE);
    const float merge_dot = 2.0f * cos_limit * cos_limit - 1.0f;

    /* A CLOSE does not have to bring the path back to where it started, and when it does not there is
     * still an edge from the last vertex to the first. The fill closes every subpath implicitly, so
     * leaving that edge out of the stroke shows up as one unstroked side — a ring sector's radial edge
     * is the usual way to hit it. Give it a segment of its own; the list then wraps, and the vertices
     * at both of its ends become ordinary corners. */
    const float gap_x = points_x[vertex_count - 1] - points_x[0], gap_y = points_y[vertex_count - 1] - points_y[0];
    const bool has_closing_segment = closed && (gap_x * gap_x + gap_y * gap_y > VEC_CLOSE_EPSILON * VEC_CLOSE_EPSILON);
    const int segment_count = closed ? (has_closing_segment ? vertex_count : vertex_count - 1) : (vertex_count - 1);

    /* The corner where the LAST segment meets segment 0 is shared by two quads built at opposite ends
     * of the loop, so it has to be decided before segment 0 is emitted. */
    bool wrap_merged = false;
    float wrap_offset_x = 0.0f, wrap_offset_y = 0.0f;
    float wrap_out_x = 0.0f, wrap_out_y = 0.0f;
    bool wrap_join = false; /* the wrap corner did not merge: its join is emitted with the last quad */
    const int wrap_vertex =
        has_closing_segment ? 0 : (vertex_count - 1); /* the shared vertex: P[0] once the list wraps */
    if (closed && segment_count >= 2)
    {
        /* The outgoing direction leaves the shared vertex toward P[1]. With a closing segment that
         * vertex IS P[0], so this is segment 0's own direction; without one it is P[vertex_count-1], which sits on
         * top of P[0] — and reading it from there is what the per-vertex join has always done. */
        const int last_start = segment_count - 1;
        const int last_end = seg_end_index(last_start, vertex_count);
        float in_x, in_y, in_length, out_x, out_y, out_length;
        if (seg_dir(points_x[last_start],
                    points_y[last_start],
                    points_x[last_end],
                    points_y[last_end],
                    &in_x,
                    &in_y,
                    &in_length)
            && seg_dir(
                points_x[wrap_vertex], points_y[wrap_vertex], points_x[1], points_y[1], &out_x, &out_y, &out_length))
        {
            wrap_out_x = out_x;
            wrap_out_y = out_y;
            wrap_merged = join_merge(
                in_x, in_y, in_length, out_x, out_y, out_length, half_width, merge_dot, &wrap_offset_x, &wrap_offset_y);
            wrap_join = !wrap_merged;
        }
    }

    /* Corner carried from the previous vertex; segment 0 of a closed subpath inherits the wrap corner
     * so both quads meeting there use the same one. */
    bool previous_merged = wrap_merged;
    float previous_offset_x = wrap_offset_x, previous_offset_y = wrap_offset_y;

    for (int segment = 0; segment < segment_count; segment++)
    {
        const int start_index = segment, end_index = seg_end_index(segment, vertex_count);
        float unit_x, unit_y, length;
        if (!seg_dir(points_x[start_index],
                     points_y[start_index],
                     points_x[end_index],
                     points_y[end_index],
                     &unit_x,
                     &unit_y,
                     &length))
        {
            previous_merged = false; /* nothing was emitted here, so the next vertex starts clean */
            continue;
        }
        const float normal_x = -unit_y * half_width, normal_y = unit_x * half_width; /* left normal * r */
        float start_x = points_x[start_index], start_y = points_y[start_index], end_x = points_x[end_index],
              end_y = points_y[end_index];
        /* Square cap: extend the open ends by half_width along the segment direction. */
        if (cap == ER_VECTOR_CAP_SQUARE && !closed)
        {
            if (segment == 0)
            {
                start_x -= unit_x * half_width;
                start_y -= unit_y * half_width;
            }
            if (segment == segment_count - 1)
            {
                end_x += unit_x * half_width;
                end_y += unit_y * half_width;
            }
        }

        /* The corner at this segment's END: shared with the next segment (or, on a closed subpath's
         * last segment, with segment 0 — decided up front so both agree). */
        bool merged = false;
        float offset_x = 0.0f, offset_y = 0.0f;
        if (closed && segment == segment_count - 1)
        {
            merged = wrap_merged;
            offset_x = wrap_offset_x;
            offset_y = wrap_offset_y;
            if (wrap_join)
                add_join(points_x[end_index],
                         points_y[end_index],
                         unit_x,
                         unit_y,
                         wrap_out_x,
                         wrap_out_y,
                         half_width,
                         join,
                         miter);
        }
        else if (segment < segment_count - 1)
        {
            const int next_end_index = seg_end_index(segment + 1, vertex_count);
            float next_unit_x, next_unit_y, next_length;
            if (seg_dir(points_x[end_index],
                        points_y[end_index],
                        points_x[next_end_index],
                        points_y[next_end_index],
                        &next_unit_x,
                        &next_unit_y,
                        &next_length))
            {
                merged = join_merge(unit_x,
                                    unit_y,
                                    length,
                                    next_unit_x,
                                    next_unit_y,
                                    next_length,
                                    half_width,
                                    merge_dot,
                                    &offset_x,
                                    &offset_y);
                if (!merged)
                    add_join(points_x[end_index],
                             points_y[end_index],
                             unit_x,
                             unit_y,
                             next_unit_x,
                             next_unit_y,
                             half_width,
                             join,
                             miter);
            }
        }

        add_quad(previous_merged ? start_x + previous_offset_x : start_x + normal_x,
                 previous_merged ? start_y + previous_offset_y : start_y + normal_y,
                 merged ? end_x + offset_x : end_x + normal_x,
                 merged ? end_y + offset_y : end_y + normal_y,
                 merged ? end_x - offset_x : end_x - normal_x,
                 merged ? end_y - offset_y : end_y - normal_y,
                 previous_merged ? start_x - previous_offset_x : start_x - normal_x,
                 previous_merged ? start_y - previous_offset_y : start_y - normal_y);

        previous_merged = merged;
        previous_offset_x = offset_x;
        previous_offset_y = offset_y;
    }

    /* Caps at open endpoints. */
    if (!closed && cap == ER_VECTOR_CAP_ROUND)
    {
        add_disc(points_x[0], points_y[0], half_width);
        add_disc(points_x[vertex_count - 1], points_y[vertex_count - 1], half_width);
    }
}

/** @brief Builds stroke outline edges for every subpath (into s_edges, honoring s_edge_y_top/s_edge_y_bottom). */
static void stroke_build(float stroke_width, int cap, int join, float miter)
{
    s_edge_count = 0;
    s_edge_truncated = false;
    for (int subpath_index = 0; subpath_index < s_subpath_count; subpath_index++)
        if (s_subpaths[subpath_index].count >= 1)
            stroke_subpath(&s_subpaths[subpath_index], stroke_width, cap, join, miter);
}

/** @brief Builds stroke geometry for all subpaths and rasterizes it (recording it when armed). */
static void stroke_shape(uint32_t color,
                         const ERVectorGradient* gradient,
                         float stroke_width,
                         int cap,
                         int join,
                         float miter,
                         int paint_index,
                         int clip_x0,
                         int clip_y0,
                         int clip_x1,
                         int clip_y1)
{
    if (stroke_width <= 0.0f)
        return;
#if ERUI_VECTOR_EDGE_CACHE
    if (VEC_RECORDING())
    {
        /* Record mode: full edge list, no clip-row drop — see fill_shape for the overflow contract. */
        s_edge_y_top = -VEC_UNBOUNDED;
        s_edge_y_bottom = VEC_UNBOUNDED;
        stroke_build(stroke_width, cap, join, miter);
        if (s_edge_truncated)
        {
            s_recording_failed = true;
        }
        else
        {
            if (!record_pass(ER_VEC_PASS_STROKE, paint_index))
                s_recording_failed = true;
            rasterize(s_edges, s_edge_count, color, gradient, 0, clip_x0, clip_y0, clip_x1, clip_y1);
            return;
        }
    }
#else
    (void)paint_index;
#endif
    s_edge_y_top = (float)clip_y0;
    s_edge_y_bottom = (float)clip_y1;
    stroke_build(stroke_width, cap, join, miter);
    rasterize(s_edges, s_edge_count, color, gradient, 0, clip_x0, clip_y0, clip_x1, clip_y1);
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
static float paint_margin(const ERVectorPaint* paint)
{
    if (!paint)
        return 1.0f;
    float margin = 1.0f;
    const bool has_stroke =
        paint->stroke_width > 0.0f && (((paint->stroke >> 24) & 0xFFU) != 0U || paint->stroke_gradient != 0);
    if (has_stroke)
    {
        const float half_width = paint->stroke_width * 0.5f;
        float reach = VEC_CAP_REACH; /* round/butt/square caps and bevel joins all stay inside 1.5 * half-width */
        if (paint->join == ER_VECTOR_JOIN_MITER)
        {
            const float miter_limit = (paint->miter > 0.0f) ? paint->miter : VEC_DEFAULT_MITER_LIMIT;
            if (miter_limit > reach)
                reach = miter_limit;
        }
        margin += half_width * reach;
    }
    return margin;
}

/**
 * @brief Decides whether a shape's ink cannot reach the clip box, reading only its op run.
 *
 * Bounds the run as it walks it — curves by their control points (a bezier stays inside its control
 * hull), arcs by the centre square — so it needs no flattening and no trig, just a few compares per
 * op. The bound only ever grows, so the moment it reaches the clip the answer is settled and the walk
 * stops: a shape that IS on screen costs a couple of ops, and only one being skipped is walked whole.
 *
 * @param[in]  ops       Op-tape.
 * @param[in]  op_index  Index of the run's first op (just past SHAPE + its paint index).
 * @param[in]  op_count  Tape length.
 * @param[in]  origin_x,origin_y  Node origin added to tape coordinates.
 * @param[in]  margin  Margin the paint adds around the path (paint_margin).
 * @param[in]  clip_x0,clip_y0,clip_x1,clip_y1  Clip box.
 * @param[out] end  Receives the tape index just past the run (only meaningful when skipping).
 *
 * @return true when the shape cannot touch the clip and may be skipped whole. False is always safe:
 *         it is also the answer for a truncated or unknown op, which the general parser then handles.
 */
static bool shape_clipped_out(const float* ops,
                              int op_index,
                              int op_count,
                              int origin_x,
                              int origin_y,
                              float margin,
                              int clip_x0,
                              int clip_y0,
                              int clip_x1,
                              int clip_y1,
                              int* end)
{
    const float reach_x0 = (float)clip_x0 - margin, reach_y0 = (float)clip_y0 - margin;
    const float reach_x1 = (float)clip_x1 + margin, reach_y1 = (float)clip_y1 + margin;
    bool found_any = false;
    float bounds_x0 = VEC_UNBOUNDED, bounds_y0 = VEC_UNBOUNDED, bounds_x1 = -VEC_UNBOUNDED, bounds_y1 = -VEC_UNBOUNDED;

    while (op_index < op_count && ops[op_index] != ER_VECTOR_OP_SHAPE)
    {
        const float opcode = ops[op_index++];
        const int argument_count = (opcode == ER_VECTOR_OP_MOVE || opcode == ER_VECTOR_OP_LINE)   ? VEC_POINT_ARGS
                                   : (opcode == ER_VECTOR_OP_QUAD)                                ? VEC_QUAD_ARGS
                                   : (opcode == ER_VECTOR_OP_CUBIC || opcode == ER_VECTOR_OP_ARC) ? VEC_CURVE_ARGS
                                   : (opcode == ER_VECTOR_OP_CLOSE)                               ? 0
                                                                                                  : -1;
        if (argument_count < 0 || op_index + argument_count > op_count)
            return false; /* unknown or truncated: let the general parser deal with it */
        if (opcode == ER_VECTOR_OP_ARC)
        {
            /* Bound the whole circle: cheaper than working out which quadrants the sweep touches, and
             * a partial arc is only ever a sub-box of it. */
            const float arc_center_x = (float)origin_x + ops[op_index];
            const float arc_center_y = (float)origin_y + ops[op_index + 1];
            const float arc_radius = fabsf(ops[op_index + 2]);
            if (arc_center_x - arc_radius < bounds_x0)
                bounds_x0 = arc_center_x - arc_radius;
            if (arc_center_y - arc_radius < bounds_y0)
                bounds_y0 = arc_center_y - arc_radius;
            if (arc_center_x + arc_radius > bounds_x1)
                bounds_x1 = arc_center_x + arc_radius;
            if (arc_center_y + arc_radius > bounds_y1)
                bounds_y1 = arc_center_y + arc_radius;
            found_any = true;
        }
        else
        {
            for (int argument = 0; argument < argument_count; argument += 2)
            {
                const float x = (float)origin_x + ops[op_index + argument];
                const float y = (float)origin_y + ops[op_index + argument + 1];
                if (x < bounds_x0)
                    bounds_x0 = x;
                if (y < bounds_y0)
                    bounds_y0 = y;
                if (x > bounds_x1)
                    bounds_x1 = x;
                if (y > bounds_y1)
                    bounds_y1 = y;
                found_any = true;
            }
        }
        op_index += argument_count;
        /* The box only grows: once it overlaps the clip, no later op can make this shape skippable. */
        if (found_any && bounds_x1 > reach_x0 && bounds_x0 < reach_x1 && bounds_y1 > reach_y0 && bounds_y0 < reach_y1)
            return false;
    }

    *end = op_index;
    return found_any;
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
    float center_x, center_y; /**< Centre in framebuffer pixels (the node origin already folded in). */
    float radius;             /**< Radius in pixels. */
    float start_deg;          /**< Sweep start, degrees clockwise from +X. */
    float sweep_deg;          /**< Sweep extent, (0, 360]. */
    bool full;                /**< Sweep covers the whole circle. */
    int end;                  /**< Tape index just past this shape's op run. */
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
 * @param[in]  ops       Op-tape.
 * @param[in]  op_index  Index of the run's first op (just past SHAPE + its paint index).
 * @param[in]  op_count  Tape length.
 * @param[in]  origin_x,origin_y  Node origin added to tape coordinates.
 * @param[out] out  Receives the arc on a match.
 *
 * @return true when the run is exactly one arc.
 */
static bool arc_run_match(const float* ops, int op_index, int op_count, int origin_x, int origin_y, VecArcRun* out)
{
    bool have_move = false, have_arc = false;
    float move_x = 0.0f, move_y = 0.0f, arc_start_x = 0.0f, arc_start_y = 0.0f;

    while (op_index < op_count && ops[op_index] != ER_VECTOR_OP_SHAPE)
    {
        const float opcode = ops[op_index];
        if (opcode == ER_VECTOR_OP_MOVE)
        {
            if (have_move || have_arc || op_index + VEC_MOVE_OP_SIZE > op_count)
                return false; /* a second subpath, or a MOVE after the arc: not a lone arc */
            move_x = (float)origin_x + ops[op_index + 1];
            move_y = (float)origin_y + ops[op_index + 2];
            have_move = true;
            op_index += VEC_MOVE_OP_SIZE;
        }
        else if (opcode == ER_VECTOR_OP_ARC)
        {
            if (have_arc || op_index + VEC_ARC_OP_SIZE > op_count)
                return false;
            const float arc_center_x = (float)origin_x + ops[op_index + 1];
            const float arc_center_y = (float)origin_y + ops[op_index + 2];
            float arc_radius = ops[op_index + 3];
            const float start_angle = ops[op_index + 4];
            const float end_angle = ops[op_index + 5];
            const bool counter_clockwise = (ops[op_index + 6] != 0.0f);
            if (arc_radius < 0.0f)
                arc_radius = -arc_radius;

            /* Normalise to a forward (clockwise, increasing-angle) sweep, matching append_arc's wrap. */
            const float drawn_sweep = arc_sweep(start_angle, end_angle, counter_clockwise);
            const float sweep = (drawn_sweep < 0.0f) ? -drawn_sweep : drawn_sweep;
            if (!(sweep > 0.0f))
                return false; /* degenerate or NaN: nothing to draw analytically */
            out->center_x = arc_center_x;
            out->center_y = arc_center_y;
            out->radius = arc_radius;
            /* The sector core works in degrees from this start: one a turn or more out is brought within a turn,
             * or a huge one would swallow the sweep added to it. */
            float start = (drawn_sweep < 0.0f) ? (start_angle + drawn_sweep) : start_angle;
            if (!(fabsf(start) < 2.0f * ER_PI))
                start = fmodf(start, 2.0f * ER_PI);
            out->start_deg = start * ER_RAD2DEG;
            out->sweep_deg = sweep * ER_RAD2DEG;
            out->full = (sweep >= 2.0f * ER_PI - VEC_FULL_TURN_EPSILON);
            arc_start_x = arc_center_x + arc_radius * cosf(start_angle);
            arc_start_y = arc_center_y + arc_radius * sinf(start_angle);
            have_arc = true;
            op_index += VEC_ARC_OP_SIZE;
        }
        else if (opcode == ER_VECTOR_OP_CLOSE)
        {
            if (!have_arc)
                return false;
            op_index += 1;
        }
        else
        {
            return false; /* a line/curve joins the arc — the general path owns joins and end caps */
        }
    }

    if (!have_arc)
        return false;
    if (have_move
        && (fabsf(move_x - arc_start_x) > VEC_ARC_START_EPSILON || fabsf(move_y - arc_start_y) > VEC_ARC_START_EPSILON))
        return false;
    out->end = op_index;
    return true;
}

/**
 * @brief Whether a paint on a matched arc run maps exactly onto the analytic core.
 *
 * @param[in] paint  The shape's paint.
 * @param[in] arc    The matched arc.
 *
 * @return true when both the fill and the stroke (whichever are present) can be drawn as sectors.
 */
static bool arc_paint_ok(const ERVectorPaint* paint, const VecArcRun* arc)
{
    if (!paint || arc->radius <= 0.0f)
        return false;
    const bool has_fill = ((paint->fill >> 24) & 0xFFU) != 0U;
    const bool has_stroke = ((paint->stroke >> 24) & 0xFFU) != 0U && paint->stroke_width > 0.0f;
    if (!has_fill && !has_stroke)
        return false; /* nothing to draw; let the general path no-op */
    /* A filled PARTIAL arc closes on a CHORD (a circular segment), while the analytic core draws a
     * SECTOR — different shapes. Only a full circle's fill (a plain disc) maps. */
    if (has_fill && !arc->full)
        return false;
    if (has_stroke)
    {
        if (paint->cap == ER_VECTOR_CAP_SQUARE)
            return false; /* the analytic core has butt and round caps only */
        if (paint->stroke_width * 0.5f >= arc->radius)
            return false; /* the band swallows the centre and self-intersects — keep the outline path */
    }
    return true;
}

/** @brief Paints a matched arc run through the analytic core: fill (disc) then stroke (ring). */
static void
arc_run_draw(const VecArcRun* arc, const ERVectorPaint* paint, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    ERArcSector sector;
    memset(&sector, 0, sizeof(sector));
    sector.center_x = arc->center_x;
    sector.center_y = arc->center_y;
    sector.start_deg = arc->start_deg;
    sector.end_deg = arc->start_deg + arc->sweep_deg;
    sector.clip_x0 = clip_x0;
    sector.clip_y0 = clip_y0;
    sector.clip_x1 = clip_x1;
    sector.clip_y1 = clip_y1;

    if (((paint->fill >> 24) & 0xFFU) != 0U)
    {
        sector.outer_radius = arc->radius; /* reached only when full (arc_paint_ok) → a disc */
        sector.inner_radius = 0.0f;
        sector.cap = ER_ARC_CAP_BUTT;
        sector.color = paint->fill;
        er_arc_fill_sector(&sector);
    }
    if (((paint->stroke >> 24) & 0xFFU) != 0U && paint->stroke_width > 0.0f)
    {
        const float half_width = paint->stroke_width * 0.5f;
        sector.outer_radius = arc->radius + half_width;
        sector.inner_radius = arc->radius - half_width;
        sector.cap = (paint->cap == ER_VECTOR_CAP_ROUND) ? ER_ARC_CAP_ROUND : ER_ARC_CAP_BUTT;
        sector.color = paint->stroke;
        er_arc_fill_sector(&sector);
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
static bool record_arc(const VecArcRun* arc, int paint_index, const ERVectorPaint* paint)
{
    ERVecCache* entry = s_recording;
    if (entry->pass_count >= ERUI_VECTOR_CACHE_PASSES)
        return false;
    ERVecPass* pass = &entry->passes[entry->pass_count++];
    pass->kind = ER_VEC_PASS_ARC;
    pass->paint = (uint16_t)paint_index;
    pass->start = 0;
    pass->count = 0;
    /* Ink reach: the radius, plus half the stroke width when one is drawn (arc_paint_ok already
     * limited caps/joins to styles that stay inside it). */
    float reach = arc->radius;
    if (((paint->stroke >> 24) & 0xFFU) != 0U && paint->stroke_width > 0.0f)
        reach += paint->stroke_width * 0.5f;
    pass->bounds_x0 = arc->center_x - reach;
    pass->bounds_y0 = arc->center_y - reach;
    pass->bounds_x1 = arc->center_x + reach;
    pass->bounds_y1 = arc->center_y + reach;
    pass->arc_center_x = arc->center_x;
    pass->arc_center_y = arc->center_y;
    pass->arc_radius = arc->radius;
    pass->arc_start_deg = arc->start_deg;
    pass->arc_sweep_deg = arc->sweep_deg;
    pass->arc_full = arc->full;
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
                        int op_count,
                        const ERVectorPaint* paints,
                        int paint_count,
                        const ERVectorGradient* gradients,
                        int gradient_count,
                        int origin_x,
                        int origin_y,
                        int clip_x0,
                        int clip_y0,
                        int clip_x1,
                        int clip_y1)
{
    if (!ops || op_count <= 0)
        return;
#if !ERUI_GRADIENT
    (void)gradients;
    (void)gradient_count;
#endif
    /* origin_x, origin_y position the geometry; the clip box bounds the rasterize compute + painting (a sub-region
     * for an interactive update, or the full node box otherwise). */
    const int clip_left = clip_x0, clip_top = clip_y0, clip_right = clip_x1, clip_bottom = clip_y1;

    int op_index = 0;
    while (op_index < op_count)
    {
        if (ops[op_index] != ER_VECTOR_OP_SHAPE)
        {
            op_index++; /* tape must start with SHAPE; skip stray values defensively */
            continue;
        }
        op_index++; /* consume SHAPE opcode */
        const float paint_value = (op_index < op_count) ? ops[op_index++] : 0.0f;
        const int paint_index =
            (paint_value >= 0.0f && paint_value < (float)paint_count) ? (int)paint_value : -1; /* NaN too */
        const ERVectorPaint* shape_paint = (paint_index >= 0 && paint_index < paint_count) ? &paints[paint_index] : 0;

        /* Clip reject: bound the shape off the tape and drop it whole when its ink cannot reach the
         * clip box. A few compares per op replaces flattening it, expanding its stroke outline, sorting
         * the edges and walking every row — all of which the rasterizer would otherwise do before the
         * clip threw the result away. This is what makes a tape of many small shapes (tick marks,
         * segments, a legend) cheap to repaint for a damage rect covering only one of them.
         * OFF while recording into the edge cache: the cache must hold every shape, whatever this
         * render's clip — replay does the equivalent skip from each pass's stored bounds. */
        if (!VEC_RECORDING())
        {
            int run_end = 0;
            if (shape_clipped_out(ops,
                                  op_index,
                                  op_count,
                                  origin_x,
                                  origin_y,
                                  paint_margin(shape_paint),
                                  clip_left,
                                  clip_top,
                                  clip_right,
                                  clip_bottom,
                                  &run_end))
            {
                op_index = run_end;
                continue;
            }
        }

#if ERUI_VECTOR_ANALYTIC_ARC
        /* Analytic fast path: a shape that is just a circular arc goes straight to the shared sector
         * core, skipping flatten + stroke-outline + scanline coverage entirely. A gradient paint keeps
         * the general path — vector gradients carry SVG axis geometry, which the sector core's
         * angle/thickness ramps do not express. */
        if (shape_paint && shape_paint->fill_gradient == 0 && shape_paint->stroke_gradient == 0)
        {
            VecArcRun arc_run;
            if (arc_run_match(ops, op_index, op_count, origin_x, origin_y, &arc_run)
                && arc_paint_ok(shape_paint, &arc_run))
            {
#if ERUI_VECTOR_EDGE_CACHE
                if (VEC_RECORDING() && !record_arc(&arc_run, paint_index, shape_paint))
                    s_recording_failed = true;
#endif
                arc_run_draw(&arc_run, shape_paint, clip_left, clip_top, clip_right, clip_bottom);
                s_analytic_arcs++;
                op_index = arc_run.end;
                continue;
            }
        }
#endif

        /* Build this shape's subpaths until the next SHAPE or end of tape. */
        s_point_count = 0;
        s_subpath_count = 0;
        float current_x = 0.0f, current_y = 0.0f;
        while (op_index < op_count && ops[op_index] != ER_VECTOR_OP_SHAPE)
        {
            const float opcode = ops[op_index++];
            /* Verify this op's args fit within the tape before reading them. A tape truncated mid-op —
             * e.g. the JS bridge capping it at ERUI_VECTOR_TAPE_MAX, or any malformed input — would
             * otherwise read past op_count into adjacent memory, producing NaN/garbage coordinates that
             * index s_cover[] out of bounds and crash. Stop parsing the shape's tail instead. */
            const int argument_count = (opcode == ER_VECTOR_OP_MOVE || opcode == ER_VECTOR_OP_LINE)   ? VEC_POINT_ARGS
                                       : (opcode == ER_VECTOR_OP_QUAD)                                ? VEC_QUAD_ARGS
                                       : (opcode == ER_VECTOR_OP_CUBIC || opcode == ER_VECTOR_OP_ARC) ? VEC_CURVE_ARGS
                                                                                                      : 0;
            if (op_index + argument_count > op_count)
                break;
            if (opcode == ER_VECTOR_OP_MOVE)
            {
                current_x = (float)origin_x + ops[op_index++];
                current_y = (float)origin_y + ops[op_index++];
                sub_begin(current_x, current_y);
            }
            else if (opcode == ER_VECTOR_OP_LINE)
            {
                current_x = (float)origin_x + ops[op_index++];
                current_y = (float)origin_y + ops[op_index++];
                pt_add(current_x, current_y);
            }
            else if (opcode == ER_VECTOR_OP_QUAD)
            {
                const float control_x = (float)origin_x + ops[op_index++];
                const float control_y = (float)origin_y + ops[op_index++];
                const float end_x = (float)origin_x + ops[op_index++];
                const float end_y = (float)origin_y + ops[op_index++];
                /* Elevate the quadratic to a cubic and reuse the cubic flattener. */
                const float control1_x = current_x + VEC_QUAD_TO_CUBIC * (control_x - current_x);
                const float control1_y = current_y + VEC_QUAD_TO_CUBIC * (control_y - current_y);
                const float control2_x = end_x + VEC_QUAD_TO_CUBIC * (control_x - end_x);
                const float control2_y = end_y + VEC_QUAD_TO_CUBIC * (control_y - end_y);
                flatten_cubic(current_x, current_y, control1_x, control1_y, control2_x, control2_y, end_x, end_y, 0);
                current_x = end_x;
                current_y = end_y;
            }
            else if (opcode == ER_VECTOR_OP_CUBIC)
            {
                const float control1_x = (float)origin_x + ops[op_index++];
                const float control1_y = (float)origin_y + ops[op_index++];
                const float control2_x = (float)origin_x + ops[op_index++];
                const float control2_y = (float)origin_y + ops[op_index++];
                const float end_x = (float)origin_x + ops[op_index++];
                const float end_y = (float)origin_y + ops[op_index++];
                flatten_cubic(current_x, current_y, control1_x, control1_y, control2_x, control2_y, end_x, end_y, 0);
                current_x = end_x;
                current_y = end_y;
            }
            else if (opcode == ER_VECTOR_OP_ARC)
            {
                const float arc_center_x = (float)origin_x + ops[op_index++];
                const float arc_center_y = (float)origin_y + ops[op_index++];
                const float arc_radius = ops[op_index++];
                const float start_angle = ops[op_index++];
                const float end_angle = ops[op_index++];
                const int counter_clockwise = (ops[op_index++] != 0.0f);
                if (s_subpath_count == 0)
                    sub_begin(arc_center_x + arc_radius * cosf(start_angle),
                              arc_center_y + arc_radius * sinf(start_angle));
                append_arc(arc_center_x, arc_center_y, arc_radius, start_angle, end_angle, counter_clockwise);
                current_x = arc_center_x + arc_radius * cosf(end_angle);
                current_y = arc_center_y + arc_radius * sinf(end_angle);
            }
            else if (opcode == ER_VECTOR_OP_CLOSE)
            {
                if (s_subpath_count > 0)
                {
                    s_subpaths[s_subpath_count - 1].closed = 1;
                    current_x = s_point_x[s_subpaths[s_subpath_count - 1].start];
                    current_y = s_point_y[s_subpaths[s_subpath_count - 1].start];
                }
            }
            else
            {
                /* Unknown opcode: stop to avoid desync. */
                break;
            }
        }

        /* Apply the shape's paint: fill first, then stroke (SVG paint order). */
        const ERVectorPaint* paint = shape_paint;
        if (paint)
        {
            const ERVectorGradient* fill_gradient = NULL;
            const ERVectorGradient* stroke_gradient = NULL;
#if ERUI_GRADIENT
            ERVectorGradient fill_resolved, stroke_resolved;
            fill_gradient =
                resolve_grad(paint->fill_gradient, gradients, gradient_count, origin_x, origin_y, &fill_resolved);
            stroke_gradient =
                resolve_grad(paint->stroke_gradient, gradients, gradient_count, origin_x, origin_y, &stroke_resolved);
#endif
            if (fill_gradient || ((paint->fill >> 24) & 0xFFU) != 0U)
                fill_shape(paint->fill,
                           fill_gradient,
                           paint->fill_rule == ER_VECTOR_FILL_EVENODD,
                           paint_index,
                           clip_left,
                           clip_top,
                           clip_right,
                           clip_bottom);
            if ((stroke_gradient || ((paint->stroke >> 24) & 0xFFU) != 0U) && paint->stroke_width > 0.0f)
                stroke_shape(paint->stroke,
                             stroke_gradient,
                             paint->stroke_width,
                             paint->cap,
                             paint->join,
                             paint->miter > 0.0f ? paint->miter : VEC_DEFAULT_MITER_LIMIT,
                             paint_index,
                             clip_left,
                             clip_top,
                             clip_right,
                             clip_bottom);
        }
    }
}

void er_vector_render(const float* ops,
                      int op_count,
                      const ERVectorPaint* paints,
                      int paint_count,
                      const ERVectorGradient* gradients,
                      int gradient_count,
                      int origin_x,
                      int origin_y,
                      int clip_x0,
                      int clip_y0,
                      int clip_x1,
                      int clip_y1)
{
    /* The tape-only entry point has no node identity to key a cache on, so it never records. */
    render_tape(ops,
                op_count,
                paints,
                paint_count,
                gradients,
                gradient_count,
                origin_x,
                origin_y,
                clip_x0,
                clip_y0,
                clip_x1,
                clip_y1);
}

#if ERUI_VECTOR_EDGE_CACHE
/**
 * @brief Paints a cache entry: replays each recorded pass whose ink bounds reach the clip.
 *
 * Colors, gradients and fill rules come from the CURRENT paint table, not the recording — the store
 * invalidates the entry on any tape/paint change, so they are the same ones the recording saw; reading
 * them fresh just avoids duplicating paint state into the entry.
 */
static void replay_cache(const ERVecCache* entry,
                         const ERVectorPaint* paints,
                         int paint_count,
                         const ERVectorGradient* gradients,
                         int gradient_count,
                         int origin_x,
                         int origin_y,
                         int clip_x0,
                         int clip_y0,
                         int clip_x1,
                         int clip_y1)
{
#if !ERUI_GRADIENT
    (void)gradients;
    (void)gradient_count;
    (void)origin_x;
    (void)origin_y;
#endif
    /* One pixel of slack around the stored bounds: coverage rounds outward to pixel cells. */
    const float reach_x0 = (float)clip_x0 - 1.0f, reach_y0 = (float)clip_y0 - 1.0f;
    const float reach_x1 = (float)clip_x1 + 1.0f, reach_y1 = (float)clip_y1 + 1.0f;
    for (int pass_index = 0; pass_index < entry->pass_count; pass_index++)
    {
        const ERVecPass* pass = &entry->passes[pass_index];
        if (pass->bounds_x1 <= reach_x0 || pass->bounds_x0 >= reach_x1 || pass->bounds_y1 <= reach_y0
            || pass->bounds_y0 >= reach_y1)
            continue; /* this pass cannot touch the clip — the cached form of shape_clipped_out */
        if ((int)pass->paint >= paint_count)
            continue; /* unreachable while invalidation is sound; never index past the table */
        const ERVectorPaint* paint = &paints[pass->paint];
        if (pass->kind == ER_VEC_PASS_ARC)
        {
#if ERUI_VECTOR_ANALYTIC_ARC
            VecArcRun arc;
            arc.center_x = pass->arc_center_x;
            arc.center_y = pass->arc_center_y;
            arc.radius = pass->arc_radius;
            arc.start_deg = pass->arc_start_deg;
            arc.sweep_deg = pass->arc_sweep_deg;
            arc.full = pass->arc_full;
            arc.end = 0; /* tape position; meaningless on replay */
            arc_run_draw(&arc, paint, clip_x0, clip_y0, clip_x1, clip_y1);
            s_analytic_arcs++;
#endif
            continue;
        }
        const ERVectorGradient* gradient = NULL;
#if ERUI_GRADIENT
        ERVectorGradient resolved;
        gradient = resolve_grad((pass->kind == ER_VEC_PASS_FILL) ? paint->fill_gradient : paint->stroke_gradient,
                                gradients,
                                gradient_count,
                                origin_x,
                                origin_y,
                                &resolved);
#endif
        if (pass->kind == ER_VEC_PASS_FILL)
            rasterize(entry->edges + pass->start,
                      pass->count,
                      paint->fill,
                      gradient,
                      paint->fill_rule == ER_VECTOR_FILL_EVENODD,
                      clip_x0,
                      clip_y0,
                      clip_x1,
                      clip_y1);
        else
            rasterize(entry->edges + pass->start,
                      pass->count,
                      paint->stroke,
                      gradient,
                      0,
                      clip_x0,
                      clip_y0,
                      clip_x1,
                      clip_y1);
    }
}
#endif /* ERUI_VECTOR_EDGE_CACHE */

void er_vector_render_slot(int slot, int origin_x, int origin_y, int clip_x0, int clip_y0, int clip_x1, int clip_y1)
{
    int op_count = 0, paint_count = 0, gradient_count = 0;
    const float* ops = er_vector_slot_ops(slot, &op_count);
    const ERVectorPaint* paints = er_vector_slot_paints(slot, &paint_count);
    const ERVectorGradient* gradients = er_vector_slot_grads(slot, &gradient_count);
    if (!ops || op_count <= 0)
        return;
    if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
        return;
#if ERUI_VECTOR_EDGE_CACHE
    const ERVecCache* hit = er_vector_cache_lookup(slot, origin_x, origin_y);
    if (hit)
    {
        replay_cache(hit,
                     paints,
                     paint_count,
                     gradients,
                     gradient_count,
                     origin_x,
                     origin_y,
                     clip_x0,
                     clip_y0,
                     clip_x1,
                     clip_y1);
        return;
    }
    /* Miss. Maybe record this render — begin() only grants an entry for a tape that stayed unchanged
     * since the previous render (two-touch), so an animated node never pays the record-mode build. */
    s_recording = er_vector_cache_begin(slot, origin_x, origin_y);
    s_recording_failed = false;
    render_tape(ops,
                op_count,
                paints,
                paint_count,
                gradients,
                gradient_count,
                origin_x,
                origin_y,
                clip_x0,
                clip_y0,
                clip_x1,
                clip_y1);
    if (s_recording)
    {
        er_vector_cache_finish(s_recording, !s_recording_failed);
        s_recording = 0;
    }
#else
    render_tape(ops,
                op_count,
                paints,
                paint_count,
                gradients,
                gradient_count,
                origin_x,
                origin_y,
                clip_x0,
                clip_y0,
                clip_x1,
                clip_y1);
#endif
}
