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

#endif
