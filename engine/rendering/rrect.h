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
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

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
