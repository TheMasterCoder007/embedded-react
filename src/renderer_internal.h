#ifndef EMBEDDED_REACT_RENDERER_INTERNAL_H
#define EMBEDDED_REACT_RENDERER_INTERNAL_H

#include "native_renderer.h"

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Returns the currently active render backend.
 *
 * @return Pointer to the active EmbeddedRenderBackend, or NULL if none has been set.
 */
const EmbeddedRenderBackend* er_backend(void);

/**
 * @brief Fills a rectangle with a solid ARGB color via the active backend.
 *
 * @param[in] argb  ARGB8888 color value (straight alpha).
 * @param[in] x     X coordinate of the rectangle's left edge.
 * @param[in] y     Y coordinate of the rectangle's top edge.
 * @param[in] w     Width of the rectangle in pixels.
 * @param[in] h     Height of the rectangle in pixels.
 */
void er_blit_fill(uint32_t argb, int x, int y, int w, int h);

/**
 * @brief Copies a region of pixels from a source buffer via the active backend.
 *
 * @param[in] src     Pointer to the source pixel buffer (ARGB8888, premultiplied).
 * @param[in] stride  Row stride of the source buffer in bytes.
 * @param[in] x       X coordinate of the destination rectangle.
 * @param[in] y       Y coordinate of the destination rectangle.
 * @param[in] w       Width of the region in pixels.
 * @param[in] h       Height of the region in pixels.
 */
void er_blit_copy(const void* src, int stride, int x, int y, int w, int h);

/**
 * @brief Blends a source buffer onto the framebuffer at a given global opacity via the active backend.
 *
 * @param[in] src     Pointer to the source pixel buffer (ARGB8888, premultiplied).
 * @param[in] stride  Row stride of the source buffer in bytes.
 * @param[in] alpha   Global opacity (0 = fully transparent, 255 = fully opaque).
 * @param[in] x       X coordinate of the destination rectangle.
 * @param[in] y       Y coordinate of the destination rectangle.
 * @param[in] w       Width of the region in pixels.
 * @param[in] h       Height of the region in pixels.
 */
void er_blit_blend(const void* src, int stride, uint8_t alpha, int x, int y, int w, int h);

#endif
