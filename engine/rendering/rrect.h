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

#ifndef EMBEDDED_REACT_RRECT_H
#define EMBEDDED_REACT_RRECT_H

#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Macros
 ---------------------------------------------------------------------------------------------------------------------*/

/* Shared by every rasterizer that draws or masks to a rounded-rect edge (rrect.c, gradient.c), so
 * they agree on whether the corner carries an anti-aliased fringe. */
#ifndef ERUI_BORDER_AA
#define ERUI_BORDER_AA 1
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Rounded-rect geometry (shared so a mask can match a fill pixel for pixel)
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief One scanline's coverage inside a rounded rectangle.
 *
 * All coordinates are offsets from the rectangle's left edge, so the same row description works for
 * any destination position. Produced by er_rrect_row().
 */
typedef struct
{
    int x0;              /**< First fully-covered column; 0 on rows clear of the left corner arcs. */
    int x1;              /**< One past the last fully-covered column; the box width on rows clear of the right arcs. */
    int left_radius;     /**< Left corner arc radius in play on this row; 0 = straight edge, no fringe. */
    int left_half_width; /**< Solid half-width from the left arc centre — the fringe walk starts here. */
    int left_row_distance;  /**< This row's distance from the left arc centre. */
    int right_radius;       /**< Right corner arc radius in play; 0 = straight edge. */
    int right_half_width;   /**< @see left_half_width */
    int right_row_distance; /**< @see left_row_distance */
} ERRRectRow;

/**
 * @brief A border with an independent thickness and colour per edge.
 *
 * A zero thickness or a zero-alpha colour leaves that edge unpainted, which is what makes a
 * bottom-only rule or the transparent-border spacing trick work.
 */
typedef struct
{
    int left;              /**< Left edge thickness in pixels. */
    int top;               /**< Top edge thickness. */
    int right;             /**< Right edge thickness. */
    int bottom;            /**< Bottom edge thickness. */
    uint32_t left_color;   /**< Left edge colour, straight-alpha ARGB8888. */
    uint32_t top_color;    /**< Top edge colour. */
    uint32_t right_color;  /**< Right edge colour. */
    uint32_t bottom_color; /**< Bottom edge colour. */
    uint8_t style;         /**< 0 = solid, 1 = dashed (8 on / 6 off), 2 = dotted (3 on / 3 off). */
} ERRRectBorder;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Clamps per-corner radii to the shape a rounded-rect fill will actually paint.
 *
 * Uniform radii keep opposite arcs tangent (the pill / capsule clamp); mixed radii scale each
 * opposing pair down proportionally so neighbouring arcs meet at most tangentially.
 *
 * Total: every radius comes back in [0, max(width, height)] for any input. Negative radii are squared off
 * before either clamp runs, and a box with no area (@p width or @p height <= 0) zeroes all four.
 *
 * Anything that MASKS to a filled rounded rect — the gradient background — must clamp through this
 * same function, or its edge disagrees with the fill's by a pixel at radii large enough to clamp.
 *
 * @param[in]     width                Box width in pixels.
 * @param[in]     height               Box height in pixels.
 * @param[in,out] radius_top_left      Top-left radius.
 * @param[in,out] radius_top_right     Top-right radius.
 * @param[in,out] radius_bottom_right  Bottom-right radius.
 * @param[in,out] radius_bottom_left   Bottom-left radius.
 */
void er_rrect_clamp_radii(int width,
                          int height,
                          int* radius_top_left,
                          int* radius_top_right,
                          int* radius_bottom_right,
                          int* radius_bottom_left);

/**
 * @brief Resolves one scanline's covered span inside a rounded rectangle.
 *
 * @param[in]  width                Box width in pixels.
 * @param[in]  height               Box height in pixels.
 * @param[in]  radius_top_left      Top-left radius, already clamped by er_rrect_clamp_radii().
 * @param[in]  radius_top_right     Top-right radius (clamped).
 * @param[in]  radius_bottom_right  Bottom-right radius (clamped).
 * @param[in]  radius_bottom_left   Bottom-left radius (clamped).
 * @param[in]  row                  Scanline index, 0 = the box's top row.
 * @param[out] out                  Receives the row's span and the corner arcs in play.
 */
void er_rrect_row(int width,
                  int height,
                  int radius_top_left,
                  int radius_top_right,
                  int radius_bottom_right,
                  int radius_bottom_left,
                  int row,
                  ERRRectRow* out);

/**
 * @brief Anti-aliasing coverage of the step-th fringe pixel stepping outward from a corner's solid edge.
 *
 * Signed-distance coverage sampled at the pixel centre, matching the arc er_rrect_row() reports.
 * Walk step upward from 0, stop at the first non-positive result, and bound the loop with
 * er_rrect_fringe_max() — several places walk a fringe, and none of them should be one monotonicity
 * bug away from spinning forever.
 *
 * @param[in] radius        Corner arc radius (ERRRectRow::left_radius / right_radius).
 * @param[in] half_width    Solid half-width at this row (ERRRectRow::left_half_width / right_half_width).
 * @param[in] row_distance  Row distance from the arc centre (ERRRectRow::left_row_distance / right_row_distance).
 * @param[in] step          Steps outward from the solid edge, starting at 0.
 *
 * @return Coverage: <= 0 past the arc (stop the walk), >= 1 fully inside (already solid — skip the
 *         pixel), otherwise the fraction to scale the source alpha by.
 */
float er_rrect_fringe_cov(int radius, int half_width, int row_distance, int step);

/**
 * @brief Hard upper bound on a fringe walk, so no walk is open-ended.
 *
 * The fringe starts at the solid edge — already at least @p radius's half-width from the arc centre — and
 * dies once the sample passes the radius, so it can never reach radius + 1 steps. Every walk carries this
 * as its loop bound and still breaks on the first non-positive coverage, which is the real stop.
 *
 * A straight edge (radius == 0) has no fringe at all and bounds to zero, so the walk does not run. That
 * case is the common one, not an edge case: on any rounded rect every row between the corners is a
 * straight edge on both sides, and on a square one every row is — entering the loop even once there
 * would spend a sqrtf per row per side to learn what the radius already says.
 *
 * @param[in] radius  Corner arc radius (ERRRectRow::left_radius / right_radius).
 *
 * @return Maximum steps the walk may take.
 */
static inline int er_rrect_fringe_max(int radius)
{
    return radius > 0 ? radius + 1 : 0;
}

/**
 * @brief Number of anti-aliased fringe pixels stepping outward from a corner's solid edge.
 *
 * The ring needs the count UP FRONT — its solid band has to stop where the inset shape's fringe
 * begins — which the fill paths never do, so they walk and break instead of paying for this pass.
 *
 * @param[in] radius        Corner arc radius (ERRRectRow::left_radius / right_radius); 0 — a straight edge — has
 *                          no fringe.
 * @param[in] half_width    Solid half-width at this row (ERRRectRow::left_half_width / right_half_width).
 * @param[in] row_distance  Row distance from the arc centre (ERRRectRow::left_row_distance / right_row_distance).
 *
 * @return Fringe pixel count; 0 when ERUI_BORDER_AA is off.
 */
int er_rrect_fringe_len(int radius, int half_width, int row_distance);

/**
 * @brief Fills a rounded rectangle with a solid color.
 *
 * A radius of 0 (or negative) renders a plain axis-aligned rectangle via a single
 * fill_rect call. A radius that would cause corner arcs to overlap is clamped so
 * the arcs remain tangent (pill / capsule shape).
 *
 * Anti-aliased corner edges are blended when ERUI_BORDER_AA is non-zero.
 * The blend relies on the backend's fill_rect treating a sub-255 alpha channel as a
 * transparency request rather than a solid overwrite.
 *
 * @param[in] argb    Fill color as straight-alpha ARGB8888.
 * @param[in] x       Left edge of the bounding box in framebuffer pixels.
 * @param[in] y       Top edge of the bounding box in framebuffer pixels.
 * @param[in] width   Width of the bounding box in pixels.
 * @param[in] height  Height of the bounding box in pixels.
 * @param[in] radius  Corner radius in pixels (0 = plain rectangle).
 */
void er_rrect_fill(uint32_t argb, int x, int y, int width, int height, int radius);

/**
 * @brief Fills a rounded rectangle with a background color and an optional border ring.
 *
 * Draws the border by painting the outer bounds in border_argb first, then
 * overpainting the inset region in background_argb. When border_width is 0 or border_argb
 * is fully transparent, only the background fill is drawn. The inner corner radius
 * is reduced by border_width to keep the inner and outer arcs concentric.
 *
 * @param[in] background_argb  Background fill color (straight-alpha ARGB8888).
 * @param[in] border_argb      Border ring color (straight-alpha ARGB8888).
 * @param[in] border_width     Border thickness in pixels (0 = no border).
 * @param[in] x                Left edge of the outer bounding box in framebuffer pixels.
 * @param[in] y                Top edge of the outer bounding box in framebuffer pixels.
 * @param[in] width            Width of the outer bounding box in pixels.
 * @param[in] height           Height of the outer bounding box in pixels.
 * @param[in] radius           Outer corner radius in pixels (0 = plain rectangle).
 */
void er_rrect_fill_bordered(
    uint32_t background_argb, uint32_t border_argb, int border_width, int x, int y, int width, int height, int radius);

/**
 * @brief Fills a rounded rectangle with independent per-corner radii.
 *
 * Each corner radius is clamped to prevent opposite arcs from overlapping.
 * A radius of 0 on a corner produces a right-angle corner for that corner only.
 * Anti-aliased edges are blended when ERUI_BORDER_AA is non-zero.
 *
 * @param[in] argb                 Fill color (straight-alpha ARGB8888).
 * @param[in] x                    Left edge in framebuffer pixels.
 * @param[in] y                    Top edge in framebuffer pixels.
 * @param[in] width                Width in pixels.
 * @param[in] height               Height in pixels.
 * @param[in] radius_top_left      Top-left corner radius in pixels.
 * @param[in] radius_top_right     Top-right corner radius in pixels.
 * @param[in] radius_bottom_right  Bottom-right corner radius in pixels.
 * @param[in] radius_bottom_left   Bottom-left corner radius in pixels.
 */
void er_rrect_fill_corners(uint32_t argb,
                           int x,
                           int y,
                           int width,
                           int height,
                           int radius_top_left,
                           int radius_top_right,
                           int radius_bottom_right,
                           int radius_bottom_left);

/**
 * @brief Strokes a rounded-rect border as a hollow ring, leaving the interior untouched.
 *
 * The alternative — filling the whole shape in the border colour and painting the background back
 * over the inset — only works when that background is fully opaque. With a transparent background
 * (React Native's default for a View) the fill has nothing to cover it and the node renders as a
 * solid block of border colour; with a translucent one the border shows through it; and with a
 * gradient background, already painted into this box, the fill hides it outright. A ring covers only
 * the band, so all three keep whatever is behind them.
 *
 * Both edges of the band are anti-aliased when ERUI_BORDER_AA is set: the outer edge fades out of
 * the border colour, the inner edge fades by the coverage the inset shape takes back.
 *
 * @param[in] argb                 Border colour as straight-alpha ARGB8888.
 * @param[in] x                    Left edge of the outer bounding box in framebuffer pixels.
 * @param[in] y                    Top edge of the outer bounding box in framebuffer pixels.
 * @param[in] width                Outer width in pixels.
 * @param[in] height               Outer height in pixels.
 * @param[in] radius_top_left      Outer top-left corner radius in pixels.
 * @param[in] radius_top_right     Outer top-right corner radius.
 * @param[in] radius_bottom_right  Outer bottom-right corner radius.
 * @param[in] radius_bottom_left   Outer bottom-left corner radius.
 * @param[in] band_width           Band thickness in pixels; <= 0 draws nothing. A band thick enough to swallow the
 *                  interior fills the whole shape, matching a plain rounded-rect fill.
 */
void er_rrect_fill_ring(uint32_t argb,
                        int x,
                        int y,
                        int width,
                        int height,
                        int radius_top_left,
                        int radius_top_right,
                        int radius_bottom_right,
                        int radius_bottom_left,
                        int band_width);

/**
 * @brief Strokes a rounded-rect border whose edges carry independent thicknesses and colours.
 *
 * The general form of er_rrect_fill_ring(), and the reason per-edge borders can follow a corner
 * radius at all: drawing them as four straight rects — as this engine used to — gives a node square
 * border corners no matter what borderRadius says.
 *
 * Adjacent edge colours meet on a mitre through each corner — the diagonal the two band widths imply,
 * 45 degrees when they are equal and tilted toward the thinner edge otherwise, which is how the web
 * splits a border corner. Inside a rounded corner that mitre becomes a radial line through the arc.
 * The seam is anti-aliased with ERUI_BORDER_AA, like every other edge here.
 *
 * A dashed or dotted style is stepped by ARC LENGTH around the perimeter, so the pattern flows
 * through the corner arcs as one continuous run and closes cleanly where it started.
 *
 * @param[in] x                    Left edge of the outer bounding box in framebuffer pixels.
 * @param[in] y                    Top edge of the outer bounding box in framebuffer pixels.
 * @param[in] width                Outer width in pixels.
 * @param[in] height               Outer height in pixels.
 * @param[in] radius_top_left      Outer top-left corner radius in pixels.
 * @param[in] radius_top_right     Outer top-right corner radius.
 * @param[in] radius_bottom_right  Outer bottom-right corner radius.
 * @param[in] radius_bottom_left   Outer bottom-left corner radius.
 * @param[in] border               Per-edge thicknesses and colours; NULL or all-empty draws nothing.
 */
void er_rrect_fill_ring_edges(int x,
                              int y,
                              int width,
                              int height,
                              int radius_top_left,
                              int radius_top_right,
                              int radius_bottom_right,
                              int radius_bottom_left,
                              const ERRRectBorder* border);

#endif
