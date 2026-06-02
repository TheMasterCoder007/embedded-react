#ifndef EMBEDDED_REACT_RENDERER_INTERNAL_H
#define EMBEDDED_REACT_RENDERER_INTERNAL_H

#include "native_renderer.h"

/* Forward declaration (full definition in er_node_internal.h) */
struct ERNode;

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
 * @brief Redirects all subsequent blit calls into an off-screen ARGB8888 scratch buffer.
 *
 * After this call er_blit_fill / er_blit_copy / er_blit_blend write pixel data into buf
 * rather than forwarding to the real backend.  Call er_scratch_end() to restore normal
 * routing.  buf must remain valid until er_scratch_end() is called.
 *
 * @param[in] buf  Scratch buffer; w × h premultiplied ARGB8888 pixels, row-major.
 * @param[in] w    Buffer width in pixels.
 * @param[in] h    Buffer height in pixels.
 * @param[in] ox   World-space X coordinate that maps to buf column 0.
 * @param[in] oy   World-space Y coordinate that maps to buf row 0.
 */
void er_scratch_begin(uint32_t* buf, int w, int h, int ox, int oy);

/**
 * @brief Clears scratch redirection, restoring blit routing to the real framebuffer.
 */
void er_scratch_end(void);

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
 * @brief Re-applies the current value of every ERAnimValue bound to this node.
 *
 * Call after er_node_set_props so a declarative prop update does not clobber a native-driver
 * animation — setProps writes the static value, then this restores the animated one in the same
 * commit. No-op for nodes with no animated bindings.
 *
 * @param[in] node  Node whose animated-bound props should be restored.
 */
void er_anim_reapply_bound(struct ERNode* node);

/**
 * @brief Advances all active layout animations by delta_ms milliseconds.
 *
 * Updates node->animated for every node with a running layout animation and marks
 * it dirty so the next er_commit() re-renders it at the interpolated position.
 *
 * @param[in] delta_ms  Milliseconds elapsed since the last tick.
 */
void er_layout_anim_tick(uint32_t delta_ms);

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

/**
 * @brief Delivers a keyboard event to the currently focused TextInput node.
 *
 * Called by embedded_renderer_key(). Inserts utf8_char into the focused node's text
 * buffer, or processes control codes such as ER_KEY_BACKSPACE and ER_KEY_RETURN.
 *
 * @param[in] keycode    Control key code (ER_KEY_BACKSPACE, ER_KEY_RETURN, …), or 0.
 * @param[in] utf8_char  Null-terminated character to insert, or NULL for control keys.
 */
void er_text_input_key(uint32_t keycode, const char* utf8_char);

#endif
