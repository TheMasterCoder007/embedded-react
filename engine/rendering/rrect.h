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
    int x0;   /**< First fully-covered column; 0 on rows clear of the left corner arcs. */
    int x1;   /**< One past the last fully-covered column; w on rows clear of the right arcs. */
    int l_r;  /**< Left corner arc radius in play on this row; 0 = straight edge, no fringe. */
    int l_dx; /**< Solid half-width from the left arc centre — the fringe walk starts here. */
    int l_dy; /**< This row's distance from the left arc centre. */
    int r_r;  /**< Right corner arc radius in play; 0 = straight edge. */
    int r_dx; /**< @see l_dx */
    int r_dy; /**< @see l_dy */
} ERRRectRow;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Clamps per-corner radii to the shape a rounded-rect fill will actually paint.
 *
 * Uniform radii keep opposite arcs tangent (the pill / capsule clamp); mixed radii scale each
 * opposing pair down proportionally so neighbouring arcs meet at most tangentially. Radii are
 * clamped in place and never go negative.
 *
 * Anything that MASKS to a filled rounded rect — the gradient background — must clamp through this
 * same function, or its edge disagrees with the fill's by a pixel at radii large enough to clamp.
 *
 * @param[in]     w      Box width in pixels.
 * @param[in]     h      Box height in pixels.
 * @param[in,out] r_tl   Top-left radius.
 * @param[in,out] r_tr   Top-right radius.
 * @param[in,out] r_br   Bottom-right radius.
 * @param[in,out] r_bl   Bottom-left radius.
 */
void er_rrect_clamp_radii(int w, int h, int* r_tl, int* r_tr, int* r_br, int* r_bl);

/**
 * @brief Resolves one scanline's covered span inside a rounded rectangle.
 *
 * @param[in]  w     Box width in pixels.
 * @param[in]  h     Box height in pixels.
 * @param[in]  r_tl  Top-left radius, already clamped by er_rrect_clamp_radii().
 * @param[in]  r_tr  Top-right radius (clamped).
 * @param[in]  r_br  Bottom-right radius (clamped).
 * @param[in]  r_bl  Bottom-left radius (clamped).
 * @param[in]  row   Scanline index, 0 = the box's top row.
 * @param[out] out   Receives the row's span and the corner arcs in play.
 */
void er_rrect_row(int w, int h, int r_tl, int r_tr, int r_br, int r_bl, int row, ERRRectRow* out);

/**
 * @brief Anti-aliasing coverage of the k-th fringe pixel stepping outward from a corner's solid edge.
 *
 * Signed-distance coverage sampled at the pixel centre, matching the arc er_rrect_row() reports.
 * Walk k upward from 0 and stop at the first non-positive result.
 *
 * @param[in] r   Corner arc radius (ERRRectRow::l_r / r_r).
 * @param[in] dx  Solid half-width at this row (ERRRectRow::l_dx / r_dx).
 * @param[in] dy  Row distance from the arc centre (ERRRectRow::l_dy / r_dy).
 * @param[in] k   Steps outward from the solid edge, starting at 0.
 *
 * @return Coverage: <= 0 past the arc (stop the walk), >= 1 fully inside (already solid — skip the
 *         pixel), otherwise the fraction to scale the source alpha by.
 */
float er_rrect_fringe_cov(int r, int dx, int dy, int k);

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
 * @param[in] w       Width of the bounding box in pixels.
 * @param[in] h       Height of the bounding box in pixels.
 * @param[in] radius  Corner radius in pixels (0 = plain rectangle).
 */
void er_rrect_fill(uint32_t argb, int x, int y, int w, int h, int radius);

/**
 * @brief Fills a rounded rectangle with a background color and an optional border ring.
 *
 * Draws the border by painting the outer bounds in border_argb first, then
 * overpainting the inset region in bg_argb. When border_w is 0 or border_argb
 * is fully transparent, only the background fill is drawn. The inner corner radius
 * is reduced by border_w to keep the inner and outer arcs concentric.
 *
 * @param[in] bg_argb     Background fill color (straight-alpha ARGB8888).
 * @param[in] border_argb Border ring color (straight-alpha ARGB8888).
 * @param[in] border_w    Border thickness in pixels (0 = no border).
 * @param[in] x           Left edge of the outer bounding box in framebuffer pixels.
 * @param[in] y           Top edge of the outer bounding box in framebuffer pixels.
 * @param[in] w           Width of the outer bounding box in pixels.
 * @param[in] h           Height of the outer bounding box in pixels.
 * @param[in] radius      Outer corner radius in pixels (0 = plain rectangle).
 */
void er_rrect_fill_bordered(
    uint32_t bg_argb, uint32_t border_argb, int border_w, int x, int y, int w, int h, int radius);

/**
 * @brief Fills a rounded rectangle with independent per-corner radii.
 *
 * Each corner radius is clamped to prevent opposite arcs from overlapping.
 * A radius of 0 on a corner produces a right-angle corner for that corner only.
 * Anti-aliased edges are blended when ERUI_BORDER_AA is non-zero.
 *
 * @param[in] argb  Fill color (straight-alpha ARGB8888).
 * @param[in] x     Left edge in framebuffer pixels.
 * @param[in] y     Top edge in framebuffer pixels.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] r_tl  Top-left corner radius in pixels.
 * @param[in] r_tr  Top-right corner radius in pixels.
 * @param[in] r_br  Bottom-right corner radius in pixels.
 * @param[in] r_bl  Bottom-left corner radius in pixels.
 */
void er_rrect_fill_corners(uint32_t argb, int x, int y, int w, int h, int r_tl, int r_tr, int r_br, int r_bl);

/**
 * @brief Draws a single border edge with an optional dash or dot pattern.
 *
 * The edge is oriented horizontally when horizontal != 0, vertically otherwise.
 * style == 0 renders a solid fill; style == 1 dashes (8 px on, 6 px off);
 * style == 2 dots (3 px on, 3 px off).
 *
 * @param[in] argb        Fill color (straight-alpha ARGB8888).
 * @param[in] style       0 = solid, 1 = dashed, 2 = dotted.
 * @param[in] x           Left edge in framebuffer pixels.
 * @param[in] y           Top edge in framebuffer pixels.
 * @param[in] w           Width in pixels.
 * @param[in] h           Height in pixels.
 * @param[in] horizontal  Non-zero to dash along the X axis; zero to dash along Y.
 */
void er_rrect_border_edge(uint32_t argb, uint8_t style, int x, int y, int w, int h, int horizontal);

#endif
