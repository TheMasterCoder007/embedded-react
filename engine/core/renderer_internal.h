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

#ifndef EMBEDDED_REACT_RENDERER_INTERNAL_H
#define EMBEDDED_REACT_RENDERER_INTERNAL_H

#include "native_renderer.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declaration (full definition in er_node_internal.h) */
struct ERNode;

/*----------------------------------------------------------------------------------------------------------------------
 - Render workers
 ---------------------------------------------------------------------------------------------------------------------*/

/* Maximum number of render workers this build supports. Every module that keeps mutable state
 * during a render pass holds it in a per-worker context array of this size, indexed by
 * er_render_worker_id(), so N workers can render disjoint screen regions concurrently without
 * sharing state. The default of 1 IS the single-core engine: the arrays have one element, the
 * worker id is the constant 0, and the compiler folds every context access back to the same
 * static addressing as before the contexts existed. */
#ifndef ERUI_RENDER_WORKERS
#define ERUI_RENDER_WORKERS 1
#endif

/**
 * @brief Returns the id of the render worker executing the current code, in [0, ERUI_RENDER_WORKERS).
 *
 * Single-worker builds (the default) always return 0 — a compile-time constant, so per-worker
 * context lookups cost nothing. Multi-worker builds resolve it through the host-installed
 * worker_id hook (see EmbeddedRenderWorkers); with no workers installed it is still 0.
 */
#if ERUI_RENDER_WORKERS > 1
int er_render_worker_id(void);
#else
static inline int er_render_worker_id(void)
{
    return 0;
}
#endif

/**
 * @brief Number of render workers a fork-join would use right now: the installed count clamped
 *        to the ERUI_RENDER_WORKERS build cap, or 1 when no workers are installed.
 */
int er_render_workers_active(void);

/**
 * @brief One worker's share of a forked render job.
 *
 * @param[in] worker  This worker's id in [0, n) — also the index of its per-worker contexts.
 * @param[in] arg     The job argument passed to er_parallel_for.
 */
typedef void (*ERParallelFn)(int worker, void* arg);

/**
 * @brief Runs fn once per active render worker and returns when every call has finished.
 *
 * Worker 0's share runs on the calling thread; remote workers are signalled FIRST (signalling
 * the calling core's share first would let it preempt the dispatch loop and serialize the whole
 * job — measured on hardware). With one active worker (the default build, no workers installed,
 * or a call from inside a worker) this is exactly fn(0, arg) — no locks, no signalling.
 */
void er_parallel_for(ERParallelFn fn, void* arg);

/**
 * @brief Number of commits rendered via the sliced parallel fork since boot.
 *
 * Diagnostic: lets hosts and tests confirm multi-core rendering is actually engaging (the fork
 * skips small damage regions and parallel-unsafe scenes).
 */
uint32_t er_parallel_frames(void);

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
 * @brief Pushes a full-extent clip entry WITHOUT intersecting the current top of stack.
 *
 * Used by offscreen source captures (transform subtree render) that must rasterise the whole
 * subtree regardless of outer scissors — the captured output is clipped by the restored stack
 * when it is blended out.  Balance with er_pop_clip_rect().
 */
void er_push_clip_reset(void);

/**
 * @brief Sets the current band strip (banded rendering): screen rows [oy, oy + h).
 *
 * Applied only at the backend-emit boundary — each backend blit is clamped to these rows and its Y
 * translated to the 0-origin band buffer. Deliberately NOT a clip-stack entry, so offscreen-scratch
 * (transform / opacity) source rendering stays complete across strip seams. Set before rendering a
 * strip; call er_set_band(0, 0) afterwards to disable (the full-framebuffer path).
 *
 * @param[in] oy  Screen-space top row of the strip (subtracted from blit destinations).
 * @param[in] h   Strip height in rows; 0 disables banding.
 */
void er_set_band(int oy, int h);

/**
 * @brief Reports the active band strip, if any (used by render_tree to cull subtrees off-strip).
 *
 * @param[out] oy  Strip top (may be NULL).
 * @param[out] h   Strip height (may be NULL).
 *
 * @return true if a band strip is active (h > 0); false in full-framebuffer mode.
 */
bool er_band_active(int* oy, int* h);

/**
 * @brief Reads the current (top-most) scissor clip rectangle.
 *
 * @param[out] x  Left edge (may be NULL).
 * @param[out] y  Top edge (may be NULL).
 * @param[out] w  Width (may be NULL).
 * @param[out] h  Height (may be NULL).
 *
 * @return true and fills the outputs when a clip is active; false when the stack is empty (no clip).
 */
bool er_get_clip_rect(int* x, int* y, int* w, int* h);

/**
 * @brief Forces the next er_commit() to repaint the whole screen (no damage clipping).
 *
 * Call after anything that invalidates the persistent framebuffer's contents — e.g. installing a
 * new render backend — so the first commit fully redraws instead of only the changed region.
 */
void er_force_full_repaint(void);

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
 * @brief Copies a KNOWN-FULLY-OPAQUE source buffer in the given pixel format via the active backend.
 *
 * The caller guarantees every source pixel is opaque (the image registry's registration-time scan,
 * or the format itself for RGB565). When the backend provides copy_rect_fmt and no engine-side
 * compositing is pending (no inherited draw alpha, no scratch capture), the buffer is handed
 * through in its source format as ONE backend call for the whole rect — on DMA2D-class hardware
 * a single M2M(_PFC) transfer.
 *
 * @param[in] src     Pointer to the source pixel buffer in @p fmt layout.
 * @param[in] stride  Row stride of the source buffer in bytes (of @p fmt).
 * @param[in] fmt     Pixel format of src.
 * @param[in] x       X coordinate of the destination rectangle.
 * @param[in] y       Y coordinate of the destination rectangle.
 * @param[in] w       Width of the region in pixels.
 * @param[in] h       Height of the region in pixels.
 *
 * @return true when the blit was fully handled (including "fully clipped, nothing to draw").
 *         false only when fmt != ER_IMG_ARGB8888 and the format-aware backend path is unavailable
 *         (no copy_rect_fmt backend, scratch capture, or inherited alpha) — the caller must then
 *         expand rows on the CPU and emit them through er_blit_copy/er_blit_blend.
 */
bool er_blit_copy_fmt(const void* src, int stride, ERImageFormat fmt, int x, int y, int w, int h);

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
 * @brief Clears every worker's backend-blit accounting accumulators (er_perf instrumentation).
 *
 * The compositor calls this when the RASTER phase opens, so whatever accumulates until
 * er_blit_perf_collect() is exactly that commit's backend-blit cost — anything a host draws
 * directly between commits (e.g. the perf overlay panel itself) is discarded here instead of
 * being mis-billed to the next frame's raster phase.
 *
 * Call only while no render workers are in flight (worker accumulators are touched unlocked).
 */
void er_blit_perf_reset(void);

/**
 * @brief Sums every worker's backend-blit accounting since the last er_blit_perf_reset().
 *
 * Reports the time spent inside backend blit callbacks (CPU time, summed across workers) and the
 * pixels handed to them (each call's final post-clip w*h). Both read 0 when ER_PERF_STATS is
 * compiled out, and the time alone reads 0 when no er_perf clock is installed.
 *
 * Call only from the commit thread after the render passes have joined.
 *
 * @param[out] us  Receives the summed callback time in microseconds. May be NULL.
 * @param[out] px  Receives the summed pixel count. May be NULL.
 */
void er_blit_perf_collect(uint32_t* us, uint32_t* px);

/**
 * @brief Sets the inherited draw alpha multiplied into every subsequent blit.
 *
 * Used by the compositor's graceful-degradation path: when a translucent group cannot be
 * composited through a scratch slot, its opacity is instead multiplied into each primitive
 * draw (exact wherever siblings don't overlap). Offscreen captures reset this to 255 for
 * the duration of the capture and re-apply it when the captured buffer is blended out.
 *
 * @param[in] alpha  Draw alpha 0–255 (255 = no dimming, the default).
 */
void er_set_draw_alpha(uint8_t alpha);

/**
 * @brief Returns the current inherited draw alpha (255 when no dimming is active).
 */
uint8_t er_get_draw_alpha(void);

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
 * @brief Clears all native animations, animation groups, and Animated.Values back to empty.
 *
 * Part of er_reset(); drops every running animation and frees every value/binding slot.
 */
void er_anim_reset(void);

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
 * @brief Removes every ERAnimValue binding that targets the given node tag.
 *
 * Call from er_node_destroy: the destroyed node's tag is recycled by the node pool, so any binding left
 * pointing at it would drive whatever node next reuses the tag (a value pushing its float to the wrong
 * element). No-op when nothing is bound to the tag.
 *
 * @param[in] node_tag  Tag of the node being destroyed.
 */
void er_anim_unbind_node(uint16_t node_tag);

/**
 * @brief Stops every animation started against the given node tag (er_anim_start), and any group holding it.
 *
 * The other half of er_anim_unbind_node, which covers only ERAnimValue bindings. Call from
 * er_node_destroy: er_anim_tick drops an animation whose node has gone, but only while the tag is still
 * on the free list — the next er_node_create recycles it, and an animation left pointing at it then
 * drives the new node instead (an <ActivityIndicator> unmounted mid-spin makes a plain View rotate).
 * Fires no completion callback; see the implementation for why. No-op when nothing targets the tag.
 *
 * @param[in] node_tag  Tag of the node being destroyed.
 */
void er_anim_cancel_node(uint16_t node_tag);

/**
 * @brief Stops any layout animation interpolating the given node tag.
 *
 * The er_layout_anim_tick half of the same recycled-tag hazard er_anim_cancel_node covers. Call from
 * er_node_destroy. No-op when the tag has no layout animation.
 *
 * @param[in] node_tag  Tag of the node being destroyed.
 */
void er_layout_anim_cancel_node(uint16_t node_tag);

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
 * @brief Clears all running layout animations and any pending LayoutAnimation config.
 *
 * Part of er_reset().
 */
void er_layout_anim_reset(void);

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
 * @brief Accepts a touch event from the host, coalescing moves to one dispatch per frame.
 *
 * The entry point behind embedded_renderer_touch(). Down, up and cancel go straight through to
 * er_dispatch_touch(); a move is parked (replacing any earlier parked move for that finger) and
 * dispatched by the next er_input_flush_moves().
 *
 * @param[in] finger_id  Finger index (0 for single-touch devices).
 * @param[in] phase      Phase of the touch event.
 * @param[in] x          X coordinate of the touch point in framebuffer pixels.
 * @param[in] y          Y coordinate of the touch point in framebuffer pixels.
 */
void er_input_queue_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);

/**
 * @brief Dispatches every finger's parked touch-move (the frame boundary for input).
 *
 * Called at the top of er_commit(), and by the QuickJS bridge at the top of its frame pump so a
 * drag's state updates are already in the scene graph when that frame lays out. Idempotent: a
 * second call in the same frame, or a call with nothing parked, does nothing. A parked move whose
 * position equals the last dispatched one is dropped rather than dispatched.
 */
void er_input_flush_moves(void);

/**
 * @brief Enables or disables touch-move coalescing (enabled by default).
 *
 * With coalescing off, every move is dispatched as it arrives — the pre-coalescing behaviour, for a
 * host that needs every sample (freehand capture) and can pay for it. Disabling flushes anything
 * already parked.
 *
 * @param[in] enabled  true to coalesce moves to one dispatch per frame, false to dispatch each one.
 */
void er_input_set_move_coalescing(bool enabled);

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

/** @brief True when the software keyboard should be drawn (enabled + a TextInput is focused). */
bool er_keyboard_active(void);

/** @brief Draws the on-screen keyboard strip over the bottom of the screen (no-op when inactive). Honours
 *         the active clip / band, so it composites on top of the scene within the current repaint region. */
void er_keyboard_draw(int screen_w, int screen_h);

/** @brief Routes a touch to the on-screen keyboard. Returns true (consumed) when the keyboard is active and
 *         the point is inside its strip — a key DOWN types into the focused input; otherwise false. */
bool er_keyboard_dispatch_touch(ERTouchPhase phase, int x, int y);

/** @brief Pixels the scene is shifted up for keyboard avoidance (0 when none); add to a touch Y for hit-testing. */
int er_keyboard_avoid_offset(void);

#endif
