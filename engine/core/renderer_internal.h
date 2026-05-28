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
 * @brief Pushes a new clip rectangle, intersected with any currently active clip.
 *
 * All subsequent er_blit_fill / er_blit_copy / er_blit_blend calls are scissored to the
 * intersection of every pushed clip rectangle.  Calls must be balanced with er_pop_clip_rect().
 *
 * @param[in] x  Left edge of the clip rectangle.
 * @param[in] y  Top edge of the clip rectangle.
 * @param[in] w  Width of the clip rectangle in pixels.
 * @param[in] h  Height of the clip rectangle in pixels.
 */
void er_push_clip_rect(int x, int y, int w, int h);

/**
 * @brief Pops the most recently pushed clip rectangle.
 *
 * After this call the previous clip (or no clip, if the stack is now empty) is restored.
 */
void er_pop_clip_rect(void);

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

/**
 * @brief Advances the renderer's internal time counter by delta_ms milliseconds.
 *
 * Called from embedded_renderer_tick() on each frame. The accumulated value is
 * returned by er_now_ms().
 *
 * @param[in] delta_ms  Milliseconds elapsed since the last tick.
 */
void er_tick(uint32_t delta_ms);

/**
 * @brief Advances active native animations by delta_ms milliseconds.
 *
 * @param[in] delta_ms  Milliseconds elapsed since the last tick.
 */
void er_anim_tick(uint32_t delta_ms);

/**
 * @brief Clears input gesture state.
 */
void er_input_reset(void);

/**
 * @brief Advances input gesture timers by delta_ms milliseconds.
 *
 * @param[in] delta_ms  Milliseconds elapsed since the last tick.
 */
void er_input_tick(uint32_t delta_ms);

/**
 * @brief Dispatches a touch event into the scene event subsystem.
 *
 * @param[in] finger_id  Finger index (0 for single-touch devices).
 * @param[in] phase      Phase of the touch event.
 * @param[in] x          X coordinate of the touch point in framebuffer pixels.
 * @param[in] y          Y coordinate of the touch point in framebuffer pixels.
 */
void er_dispatch_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);

#endif
