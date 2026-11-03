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
 * @brief One annular sector to rasterize analytically: a ring [inner_radius, outer_radius] cut to the sweep
 *        [start_deg, end_deg].
 *
 * Angles are degrees, clockwise from +X in screen space (y down). Coordinates are framebuffer pixels with
 * the pixel (i, j) sampled at its centre (i + 0.5, j + 0.5) — the same convention the rounded-rect
 * rasterizer uses, so an arc and a View border of the same radius agree on where their edges fall.
 * Every edge (both ring radii, both boundary rays, round caps, segment gaps) is anti-aliased by the
 * signed distance of the pixel centre to that edge.
 */
typedef struct
{
    float center_x;     /**< Centre X in framebuffer pixels. */
    float center_y;     /**< Centre Y in framebuffer pixels. */
    float outer_radius; /**< Outer radius in pixels. */
    float inner_radius; /**< Inner radius in pixels; <= 0 draws a full sector (pie slice). */
    float start_deg;    /**< Sweep start angle in degrees. */
    float end_deg;      /**< Sweep end angle in degrees; end_deg - start_deg in [0, 360]. */
    uint8_t cap;        /**< ERArcCap: round caps add a disc of radius (outer_radius - inner_radius) / 2 at each end. */

    uint32_t color; /**< Straight-alpha ARGB8888 fill (used when gradient_type is not conic / radial). */

    /* Optional gradient paint. ER_GRADIENT_CONIC maps the angle range [gradient_start_deg, gradient_end_deg] onto the
     * stops; ER_GRADIENT_RADIAL maps the thickness [inner_radius, outer_radius]. Any other type paints @p color. */
    uint8_t gradient_type;       /**< ERGradientType. */
    uint8_t gradient_stop_count; /**< Stops in @p gradient_stops (>= 2 to take effect). */
    const ERGradientStop* gradient_stops;
    float gradient_start_deg; /**< Conic: angle where the first stop lands (degrees). */
    float gradient_end_deg;   /**< Conic: angle where the last stop lands (degrees). */

    /* Optional segment mask: the span [segment_start_deg, segment_end_deg] is split into @p segments equal pieces
     * separated by @p gap_deg; pixels inside a gap are masked out. Applied to whatever sweep this sector draws, so a
     * track and the indicator drawn over it share one mask. segments <= 1 disables it. */
    uint8_t segments;
    float segment_start_deg;
    float segment_end_deg;
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
 * @param[in] sector  Sector description (see ERArcSector). NULL or a zero sweep draws nothing.
 */
void er_arc_fill_sector(const ERArcSector* sector);

/**
 * @brief Half-chord of a circle of radius @p radius on the row whose centre lies @p row_offset pixels from the circle
 *        centre, served from the shared per-radius row-span cache when it fits.
 *
 * @param[in] radius      Circle radius in pixels (> 0).
 * @param[in] row_offset  Signed row offset from the centre in pixels.
 *
 * @return sqrt(radius² - row_offset²), or a negative value when the row misses the circle.
 */
float er_arc_half_chord(float radius, float row_offset);

/**
 * @brief Drops every cached row span (er_reset / tests).
 */
void er_arc_span_cache_reset(void);

/**
 * @brief Number of row-span cache misses since the last reset — a diagnostic for the cache's hit rate.
 */
uint32_t er_arc_span_cache_misses(void);

/**
 * @brief Grows an axis-aligned bbox with the extent of an annular sector: the sweep [start_deg, end_deg] between the
 *        radii, padded by @p padding pixels on every side.
 *
 * Used to bound the damage of a value change to the swept sub-arc. Degrees, clockwise from +X, same
 * convention as ERArcSector. The bbox is in the same pixel space as the centre.
 *
 * @param[in]     center_x,center_y  Centre.
 * @param[in]     inner_radius       Inner radius (>= 0).
 * @param[in]     outer_radius       Outer radius.
 * @param[in]     start_deg,end_deg  Sweep (either order).
 * @param[in]     padding            Extra pixels on every side (AA fringe, caps, knob).
 * @param[in,out] x0,y0              Bbox min (floats; pass a large value to start empty).
 * @param[in,out] x1,y1              Bbox max.
 */
void er_arc_sector_bbox(float center_x,
                        float center_y,
                        float inner_radius,
                        float outer_radius,
                        float start_deg,
                        float end_deg,
                        float padding,
                        float* x0,
                        float* y0,
                        float* x1,
                        float* y1);

#endif
