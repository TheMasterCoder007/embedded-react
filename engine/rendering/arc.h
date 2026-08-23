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

#ifndef EMBEDDED_REACT_ARC_H
#define EMBEDDED_REACT_ARC_H

#include "er_scene.h"
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Macros
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Entries in the per-radius row-span cache shared by every arc sector drawn (any node, any frame). */
#ifndef ERUI_ARC_SPAN_CACHE
#define ERUI_ARC_SPAN_CACHE 8
#endif

/** @brief Largest radius (px) the row-span cache serves; larger circles fall back to a per-row sqrt. */
#ifndef ERUI_ARC_MAX_RADIUS
#define ERUI_ARC_MAX_RADIUS 255
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One annular sector to rasterize analytically: a ring [r_inner, r_outer] cut to the sweep [a0, a1].
 *
 * Angles are degrees, clockwise from +X in screen space (y down). Coordinates are framebuffer pixels with
 * the pixel (i, j) sampled at its centre (i + 0.5, j + 0.5) — the same convention the rounded-rect
 * rasterizer uses, so an arc and a View border of the same radius agree on where their edges fall.
 * Every edge (both ring radii, both boundary rays, round caps, segment gaps) is anti-aliased by the
 * signed distance of the pixel centre to that edge.
 */
typedef struct
{
    float cx;      /**< Centre X in framebuffer pixels. */
    float cy;      /**< Centre Y in framebuffer pixels. */
    float r_outer; /**< Outer radius in pixels. */
    float r_inner; /**< Inner radius in pixels; <= 0 draws a full sector (pie slice). */
    float a0;      /**< Sweep start angle in degrees. */
    float a1;      /**< Sweep end angle in degrees; a1 - a0 in [0, 360]. */
    uint8_t cap;   /**< ERArcCap: round caps add a disc of radius (r_outer - r_inner) / 2 at each end. */

    uint32_t color; /**< Straight-alpha ARGB8888 fill (used when grad_type is not conic / radial). */

    /* Optional gradient paint. ER_GRADIENT_CONIC maps the angle range [grad_a0, grad_a1] onto the stops;
     * ER_GRADIENT_RADIAL maps the thickness [r_inner, r_outer]. Any other type paints @p color. */
    uint8_t grad_type;       /**< ERGradientType. */
    uint8_t grad_stop_count; /**< Stops in @p grad_stops (>= 2 to take effect). */
    const ERGradientStop* grad_stops;
    float grad_a0; /**< Conic: angle where the first stop lands (degrees). */
    float grad_a1; /**< Conic: angle where the last stop lands (degrees). */

    /* Optional segment mask: the span [seg_a0, seg_a1] is split into @p segments equal pieces separated by
     * @p gap_deg; pixels inside a gap are masked out. Applied to whatever sweep this sector draws, so a
     * track and the indicator drawn over it share one mask. segments <= 1 disables it. */
    uint8_t segments;
    float seg_a0;
    float seg_a1;
    float gap_deg;

    /* Rasterize only rows / columns inside [clip_x0, clip_x1) x [clip_y0, clip_y1). */
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
} ERArcSector;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Rasterizes one annular sector through the active render backend.
 *
 * Per row the ring's covered columns come from four half-chords (outer and inner radius, each ± half a
 * pixel) — the rounded-rect row-span idea applied to a circle — so only the one-pixel fringe at each
 * radius needs a per-pixel distance; the interior is span-filled. The boundary rays and any caps / gaps
 * are two cross products per pixel. Output is premultiplied ARGB blended in chunks with er_blit_blend.
 *
 * @param[in] s  Sector description (see ERArcSector). NULL or a zero sweep draws nothing.
 */
void er_arc_fill_sector(const ERArcSector* s);

/**
 * @brief Half-chord of a circle of radius @p r on the row whose centre lies @p dy pixels from the circle
 *        centre, served from the shared per-radius row-span cache when it fits.
 *
 * @param[in] r   Circle radius in pixels (> 0).
 * @param[in] dy  Signed row offset from the centre in pixels.
 *
 * @return sqrt(r² - dy²), or a negative value when the row misses the circle.
 */
float er_arc_half_chord(float r, float dy);

/**
 * @brief Drops every cached row span (er_reset / tests).
 */
void er_arc_span_cache_reset(void);

/**
 * @brief Number of row-span cache misses since the last reset — a diagnostic for the cache's hit rate.
 */
uint32_t er_arc_span_cache_misses(void);

/**
 * @brief Grows an axis-aligned bbox with the extent of an annular sector: the sweep [a0, a1] between the
 *        radii, padded by @p pad pixels on every side.
 *
 * Used to bound the damage of a value change to the swept sub-arc. Degrees, clockwise from +X, same
 * convention as ERArcSector. The bbox is in the same pixel space as the centre.
 *
 * @param[in]     cx,cy    Centre.
 * @param[in]     r_in     Inner radius (>= 0).
 * @param[in]     r_out    Outer radius.
 * @param[in]     a0,a1    Sweep (either order).
 * @param[in]     pad      Extra pixels on every side (AA fringe, caps, knob).
 * @param[in,out] x0,y0    Bbox min (floats; pass a large value to start empty).
 * @param[in,out] x1,y1    Bbox max.
 */
void er_arc_sector_bbox(float cx,
                        float cy,
                        float r_in,
                        float r_out,
                        float a0,
                        float a1,
                        float pad,
                        float* x0,
                        float* y0,
                        float* x1,
                        float* y1);

#endif
