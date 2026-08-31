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

#ifndef EMBEDDED_REACT_NATIVE_RENDERER_H
#define EMBEDDED_REACT_NATIVE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*----------------------------------------------------------------------------------------------------------------------
     - Types
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Pixel format of a source buffer handed to a backend blit (and of registered images).
     */
    typedef enum
    {
        ER_IMG_ARGB8888 = 0, /**< 32-bit premultiplied ARGB (0xAARRGGBB), 4 bytes per pixel. */
        ER_IMG_RGB565 = 1,   /**< 16-bit RGB565, 2 bytes per pixel; inherently fully opaque. */
    } ERImageFormat;

    /**
     * @brief Platform-supplied rendering callbacks.
     *
     * The host application fills this struct and passes it to embedded_renderer_set_backend().
     * All function pointers are optional; NULL pointers are silently ignored by the blit helpers —
     * but a backend without blend_rect cannot anti-alias: every soft edge the engine draws (vector
     * and arc coverage rows, gradients, shadows, transformed and translucent content) is emitted as
     * a premultiplied row through it, and is silently dropped when it is NULL.
     */
    typedef struct EmbeddedRenderBackend
    {
        /** @brief Fill a solid-color rectangle. argb is straight-alpha ARGB8888. */
        void (*fill_rect)(uint32_t argb, int x, int y, int w, int h, void* ctx);

        /** @brief Copy a premultiplied ARGB8888 buffer into the framebuffer. */
        void (*copy_rect)(const void* src, int src_stride_bytes, int x, int y, int w, int h, void* ctx);

        /** @brief Blend a premultiplied ARGB8888 buffer into the framebuffer at the given global alpha.
         *         Required for anti-aliased output — see the note above. */
        void (*blend_rect)(const void* src, int src_stride_bytes, uint8_t alpha, int x, int y, int w, int h, void* ctx);

        /** @brief Block until the hardware has finished consuming the last frame. May be NULL. */
        void (*wait)(void* ctx);

        /** @brief Signal that a new frame is ready for display. May be NULL. */
        void (*frame_ready)(void* ctx);

        /** @brief Opaque context pointer forwarded to every callback. */
        void* ctx;

        /*------------------------------------------------------------------------------------------------
         - Banded rendering (optional). A backend with no RAM for a full framebuffer keeps only a small
           band buffer of `band_height` rows and relies on the panel's own GRAM to retain the rest of the
           frame. When band_height > 0 the engine renders each commit's damage region as horizontal
           strips: for every strip it calls band_begin(), emits the (Y-translated) fill/copy/blend ops for
           that strip into the band buffer, then calls band_flush() to push it to the panel. The fill/copy/
           blend callbacks receive BAND-LOCAL Y (already offset by the strip's top), so the band buffer is
           a plain `screen_w x band_height` surface. Leave band_height = 0 (and these NULL) for the
           classic full-framebuffer path — the engine behaviour is then unchanged.
         ------------------------------------------------------------------------------------------------*/

        /** @brief Rows per band buffer; > 0 enables banded rendering. 0 = full-framebuffer mode. */
        int band_height;

        /** @brief Begin a strip [x,y,w,h] (screen space): clear the band buffer, remember the rect. */
        void (*band_begin)(int x, int y, int w, int h, void* ctx);

        /** @brief Flush the strip begun by band_begin() to the panel (convert + DMA). */
        void (*band_flush)(void* ctx);

        /*------------------------------------------------------------------------------------------------
         - Format-aware opaque copy (optional). The engine hands a KNOWN-FULLY-OPAQUE source buffer to
           the backend in its registered pixel format, as one call for the whole rect. Contract:
             * fmt names src's pixel layout; src_stride_bytes is the row stride in BYTES of that format.
             * The callback may be invoked with ER_IMG_ARGB8888 or ER_IMG_RGB565; implementations must
               handle both (ARGB sources may be forwarded to copy_rect).
             * Every source pixel is opaque — the engine only routes buffers its registration-time scan
               (or the format itself, for RGB565) proved opaque. Replace destination pixels outright:
               no per-pixel alpha inspection, no read-modify-write, no opacity pre-scan needed.
             * Coordinates match the other blit callbacks: in banded mode, y is band-local (already
               offset by the strip's top).
             * On DMA2D-class hardware this maps to a single M2M(_PFC) transfer (e.g. FGPFCCR color
               mode RGB565 with an ARGB8888/RGB565 output); on an RGB565 framebuffer a 565 source is
               a plain row memcpy.
           Leave NULL to keep the classic path: the engine expands non-ARGB sources on the CPU and
           emits ARGB8888 through copy_rect — behaviour is then unchanged.
         ------------------------------------------------------------------------------------------------*/

        /** @brief Copy a fully opaque src in the given format into the framebuffer (replace). */
        void (*copy_rect_fmt)(
            const void* src, int src_stride_bytes, ERImageFormat fmt, int x, int y, int w, int h, void* ctx);
    } EmbeddedRenderBackend;

    /**
     * @brief Phase of a touch event reported to the renderer.
     */
    typedef enum
    {
        ER_TOUCH_DOWN,   /**< Finger made contact with the screen. */
        ER_TOUCH_MOVE,   /**< Finger moved while in contact. */
        ER_TOUCH_UP,     /**< Finger lifted from the screen. */
        ER_TOUCH_CANCEL, /**< Touch sequence cancelled (e.g. system interrupt). */
    } ERTouchPhase;

    /*----------------------------------------------------------------------------------------------------------------------
     - Functions: Public
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Registers a platform render backend and initialises the font subsystem.
     *
     * Must be called once before any rendering or font loading.
     *
     * @param[in] backend  Pointer to the backend descriptor to activate.
     */
    void embedded_renderer_set_backend(const EmbeddedRenderBackend* backend);

    /**
     * @brief Host-provided render workers for multi-core rendering (opt-in, experimental).
     *
     * The engine never creates threads. A host that wants the renderer to use extra cores
     * provides them here: worker 0 is always the thread that calls er_commit(), and workers
     * 1..count-1 are host-owned threads (e.g. pinned FreeRTOS tasks, pthreads) that sit idle
     * until dispatched. When the engine forks a render job it calls dispatch(k) for each extra
     * worker, runs its own share on the calling thread, then calls sync() to join.
     *
     * Contract:
     *  - dispatch(k): cause er_render_worker_exec(k) to be called as soon as possible on
     *    worker k's thread, then return without waiting for it.
     *  - sync(): return only after every er_render_worker_exec() call triggered since the
     *    previous sync() has returned. dispatch()/sync() must order memory like a semaphore
     *    (any real OS primitive does), so job state written before dispatch is visible to the
     *    worker and the worker's writes are visible after sync.
     *  - worker_id(): the calling thread's worker index — 0 for the render thread, k for the
     *    thread that services dispatch(k). Called from render code only (never from other
     *    threads). On core-pinned workers this can be as cheap as reading the core id.
     *
     * Ignored (single-core rendering) unless the engine was built with ERUI_RENDER_WORKERS
     * greater than 1; count is clamped to that build cap. Install before the first commit and
     * leave installed; pass NULL to return to single-core rendering.
     */
    typedef struct EmbeddedRenderWorkers
    {
        int count;                               /**< Total workers including worker 0 (the render thread). */
        void (*dispatch)(int worker, void* ctx); /**< Signal worker k to run er_render_worker_exec(k). */
        void (*sync)(void* ctx);                 /**< Wait for all dispatched workers to finish. */
        int (*worker_id)(void);                  /**< Calling thread's worker index. */
        void* ctx;                               /**< Opaque host context passed to dispatch/sync. */
    } EmbeddedRenderWorkers;

    /**
     * @brief Installs (or removes, with NULL) the host's render workers.
     *
     * A no-op in builds with ERUI_RENDER_WORKERS == 1 (the default): the renderer then always
     * runs single-core, exactly as if this function were never called.
     */
    void embedded_renderer_set_workers(const EmbeddedRenderWorkers* workers);

    /**
     * @brief Runs the engine's pending render job share for worker k.
     *
     * Called by the HOST from worker k's thread in response to a dispatch(k) — never by
     * application code directly, and never for worker 0.
     */
    void er_render_worker_exec(int worker);

    /**
     * @brief Advances the renderer by one time step.
     *
     * Call this once per display refresh from the application main loop.
     *
     * @param[in] delta_ms  Milliseconds elapsed since the last call.
     */
    void embedded_renderer_tick(uint32_t delta_ms);

    /**
     * @brief Delivers a touch event to the renderer's input subsystem.
     *
     * Down, up and cancel are dispatched immediately. Moves are COALESCED: the renderer keeps only the
     * newest one per finger and dispatches it once per frame, at the top of er_commit() (and, on hosts
     * running the QuickJS bridge, at the top of the frame pump, which comes first). Call this as often
     * as the panel or window system reports — a drag then costs one handler run per frame instead of
     * one per sample, and the sample that survives is the newest, which is the one the frame paints.
     * A move that repeats the last dispatched position is dropped entirely, so a finger held still
     * costs nothing. Use embedded_renderer_set_touch_coalescing() to opt out.
     *
     * @param[in] finger_id  Finger index (0 for single-touch devices).
     * @param[in] phase      Phase of the touch event.
     * @param[in] x          X coordinate of the touch point in framebuffer pixels.
     * @param[in] y          Y coordinate of the touch point in framebuffer pixels.
     */
    void embedded_renderer_touch(uint8_t finger_id, ERTouchPhase phase, int x, int y);

    /**
     * @brief Dispatches the newest pending touch-move for every finger right now.
     *
     * er_commit() already does this at the top of each frame, so a normal host loop never needs to
     * call it. Reach for it when input must land at a different point than the commit — a host that
     * runs its own logic between polling the panel and committing, or a test driving a drag one
     * synthetic frame at a time. Doing nothing when nothing is pending, it is safe to call anywhere.
     */
    void embedded_renderer_flush_touch(void);

    /**
     * @brief Turns touch-move coalescing on or off (on by default).
     *
     * Off restores the pre-coalescing behaviour: every move passed to embedded_renderer_touch() is
     * dispatched as it arrives. That is what a host capturing freehand input wants, and it costs a
     * full handler run (and React render) per sample. Anything already pending is flushed first.
     *
     * @param[in] enabled  true to coalesce moves to one per frame, false to dispatch every move.
     */
    void embedded_renderer_set_touch_coalescing(bool enabled);

/** @brief Keycode for the Backspace key. */
#define ER_KEY_BACKSPACE 8U
/** @brief Keycode for the Return / Enter key. */
#define ER_KEY_RETURN 13U
/** @brief Keycode for the Escape key. */
#define ER_KEY_ESCAPE 27U
/** @brief Keycode for the Delete key (forward-delete). */
#define ER_KEY_DELETE 127U

    /**
     * @brief Delivers a keyboard event to the currently focused TextInput node.
     *
     * When a TextInput is focused (via er_text_input_focus()), calling this function
     * inserts utf8_char into the text buffer (for printable characters), or processes
     * control codes such as ER_KEY_BACKSPACE and ER_KEY_RETURN.
     *
     * @param[in] keycode    Key code (use ER_KEY_* macros for control keys, or 0 for
     *                       pure UTF-8 character input).
     * @param[in] utf8_char  Null-terminated UTF-8 encoded character to insert, or NULL
     *                       for pure control keys (ER_KEY_BACKSPACE, ER_KEY_RETURN, …).
     */
    void embedded_renderer_key(uint32_t keycode, const char* utf8_char);

#ifdef __cplusplus
}
#endif

#endif
