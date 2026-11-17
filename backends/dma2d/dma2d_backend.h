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

#ifndef EMBEDDED_REACT_DMA2D_BACKEND_H
#define EMBEDDED_REACT_DMA2D_BACKEND_H

/*
 * STM32 DMA2D (Chrom-ART) render backend.
 *
 * Drives a persistent framebuffer (typically in SDRAM, scanned out by the LTDC) with the DMA2D
 * blitter wherever the peripheral can express the engine's op exactly, and a CPU compositor
 * everywhere else:
 *
 *   fill_rect, alpha == 255  -> DMA2D register-to-memory fill (R2M)
 *   copy_rect, source opaque -> DMA2D memory-to-memory with pixel-format conversion (M2M/PFC)
 *   anything with alpha      -> CPU source-over (DMA2D's blender takes straight-alpha sources;
 *                               the engine hands premultiplied ones, so hardware blending would
 *                               double-multiply the color channels)
 *
 * The backend is SDK-free: it talks to the peripheral through its own register map, which is
 * identical on every STM32 with DMA2D (F4/F7/H7/U5). The caller supplies the peripheral base
 * address plus optional hooks for interrupt-driven start/wait and D-cache maintenance, so it
 * drops into a bare-metal loop, an RTOS task, or firmware that owns the peripheral behind a
 * service interface.
 *
 * The size comes from the engine: call er_set_display_rotation() with the panel's size first.
 *
 * Double/triple-buffered LTDC panels: see README.md — call er_set_display_buffer_count() once
 * at init, flip buffers with er_dma2d_backend_set_framebuffer(), and report each flip with
 * er_display_present(). What a buffer missed is copied from the one set before it, so a change is
 * rendered once. Flip only after er_dma2d_backend_present() returns true.
 *
 * Rotated panels, one of:
 *  - `band_buffer`: the engine draws each damage rect in strips into a small buffer, and each strip is
 *    copied into the framebuffer turned. Every LTDC framebuffer stays a scan buffer, so the panel
 *    page-flips as an unrotated one does.
 *  - `render_buffer`: the engine draws into a whole buffer of the logical shape, and
 *    er_dma2d_backend_present() copies each frame's damage into the framebuffer turned. Scan-out is
 *    single-buffered when the second LTDC framebuffer is the render buffer.
 */

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

    /** @brief Framebuffer pixel format (matches the DMA2D output PFC color-mode encoding). */
    typedef enum
    {
        ER_DMA2D_FRAMEBUFFER_ARGB8888 = 0, /**< 4 bytes/pixel, 0xAARRGGBB. */
        ER_DMA2D_FRAMEBUFFER_RGB888 = 1,   /**< 3 bytes/pixel, packed B,G,R in ascending addresses. */
        ER_DMA2D_FRAMEBUFFER_RGB565 = 2,   /**< 2 bytes/pixel. */
    } ErDma2dFramebufferFormat;

    /**
     * @brief Configuration for er_dma2d_backend_init.
     *
     * Only `dma2d`, `framebuffer` and `format` are required, plus `band_buffer` or `render_buffer` when the
     * display is rotated; every hook may be NULL. A stride of 0 means tightly packed.
     */
    typedef struct
    {
        volatile void* dma2d; /**< DMA2D peripheral. The register layout is common to all STM32 families with DMA2D. */

        void* framebuffer;               /**< The framebuffer the LTDC scans, at the panel's size (the OFF-screen
                                              buffer on page-flipped panels). */
        int stride_pixels;               /**< Framebuffer row pitch in pixels (>= panel width); 0 = tightly
                                              packed. LTDC framebuffers are commonly padded to a 64-byte row. */
        ErDma2dFramebufferFormat format; /**< Pixel format of the framebuffer and the render buffer. */

        void* render_buffer;      /**< Buffer of the logical shape the engine draws into, copied to the
                                       framebuffer by er_dma2d_backend_present(). NULL draws straight into the
                                       framebuffer, or through `band_buffer`. A second LTDC framebuffer works
                                       when it holds logical height x render stride pixels. */
        int render_stride_pixels; /**< Render buffer row pitch in pixels (>= logical width); 0 = tightly packed. */

        void* band_buffer; /**< Buffer of `band_pixels` pixels in the framebuffer format, aligned to 4 bytes.
                                The engine draws each damage rect into it in strips, and each strip is copied
                                into the framebuffer turned, so a rotated panel page-flips with no render
                                buffer. Not with `render_buffer`. Best in fast internal RAM. */
        int band_pixels;   /**< Size of the band buffer in pixels (>= logical width). A strip is as wide as
                                its damage rect and as tall as fits, so more pixels means fewer strips. */

        /*------------------------------------------------------------------------------------------------
         - Optional hooks. With `start`/`wait_complete` NULL the backend starts transfers itself and
           polls the TCIF flag — correct on any chip, no interrupts required. Provide both hooks when
           the DMA2D interrupt is owned elsewhere (e.g. firmware exposes the peripheral as a service):
           `start` is called after the backend has programmed every register EXCEPT setting CR.START;
           `wait_complete` must block until that transfer finished.
         ------------------------------------------------------------------------------------------------*/

        void (*start)(void* user);         /**< Kick off the programmed transfer (set CR.START, typically
                                                with TC/TE interrupts enabled). NULL = backend sets START. */
        void (*wait_complete)(void* user); /**< Block until the started transfer completed. NULL = poll TCIF. */

        /*------------------------------------------------------------------------------------------------
         - D-cache maintenance (Cortex-M7 with cacheable framebuffer/scratch memory). Leave NULL when
           the framebuffer region is non-cacheable or write-through (common MPU setups). See README.
         ------------------------------------------------------------------------------------------------*/

        void (*cache_clean)(const void* address, size_t bytes);      /**< Clean (flush) CPU-written lines the
                                                                       DMA2D is about to read. */
        void (*cache_clean_invalidate)(void* address, size_t bytes); /**< Clean+invalidate lines the DMA2D is
                                                                       about to overwrite. */

        void* user; /**< Forwarded to start/wait_complete. */

        uint32_t dead_time; /**< AHB dead-time cycles between DMA2D accesses (AMTCR), so large blits
                                 don't monopolize the bus against the LTDC. 0 disables. */
        int min_dma_pixels; /**< Ops smaller than this many pixels stay on the CPU (register setup
                                 costs more than a tiny blit). 0 = a sensible default. */
    } ErDma2dBackendConfig;

    /*----------------------------------------------------------------------------------------------------------------------
     - Functions: Public
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Initialises the DMA2D backend and registers it with the engine.
     *
     * @param[in] config  Backend configuration (copied).
     *
     * @return true on success; false on a bad config (no peripheral or framebuffer, a stride too narrow,
     *         no band or render buffer at a rotation other than 0, both of them, a band buffer smaller
     *         than one logical row) or when no display size is set.
     */
    bool er_dma2d_backend_init(const ErDma2dBackendConfig* config);

    /**
     * @brief Retargets rendering to another framebuffer (page flip on double/triple-buffered LTDC).
     *
     * Waits for any in-flight transfer into the old buffer first. Call between er_commit() calls,
     * then er_display_present() to advance the engine's damage rotation. With a band buffer this
     * changes where strips land; with a render buffer, where er_dma2d_backend_present() copies to.
     * The buffer it replaces becomes the source for bringing the new one up to date.
     *
     * @param[in] framebuffer  The new framebuffer (same size/format as configured).
     */
    void er_dma2d_backend_set_framebuffer(void* framebuffer);

    /** @brief Returns the current framebuffer. */
    void* er_dma2d_backend_framebuffer(void);

    /**
     * @brief Blocks until the last DMA2D transfer issued by this backend has completed.
     *
     * Call before the CPU reads the framebuffer or before handing it to the display controller.
     * Safe to call when nothing is pending.
     */
    void er_dma2d_backend_wait(void);

    /**
     * @brief Finishes the frame: waits for the DMA2D, then takes and clears the dirty box.
     *
     * The box covers every pixel drawn since the previous call (DMA2D or CPU), across commits. With a
     * render buffer it is copied into the framebuffer, turned by the display rotation. With a band or
     * render buffer the D-cache is cleaned over the box. Call once per frame, before a flip.
     *
     * @param[out] x,y,width,height  The rect in panel coordinates (untouched when it returns false). Each
     *                               may be NULL.
     *
     * @return true if anything was drawn since the last call.
     */
    bool er_dma2d_backend_present(int* x, int* y, int* width, int* height);

    /** @brief Unregisters nothing (the engine keeps its backend pointer) but clears internal state. */
    void er_dma2d_backend_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
