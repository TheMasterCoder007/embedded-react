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

#ifndef EMBEDDED_REACT_SOFTWARE_BACKEND_H
#define EMBEDDED_REACT_SOFTWARE_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     * Software (CPU) render backend.
     *
     * A platform-independent EmbeddedRenderBackend that composites the engine's fill/copy/blend ops into a
     * plain ARGB8888 framebuffer in RAM — no GPU, no OS, no display. It is the reference compositor: it runs
     * the identical primitive code the device runs, so its output is pixel-accurate to a hardware ARGB
     * target. The framebuffer it owns can be handed to any present layer (a window blit, a PNG dump, or the
     * WASM simulator's canvas — see backends/web).
     *
     * The engine repaints only each commit's damage region, so the framebuffer persists between frames and
     * stands in for a panel's GRAM. Initialised to opaque black.
     */

    /**
     * @brief Allocates the framebuffer and registers the software backend with the engine.
     *
     * Calls embedded_renderer_set_backend() internally (which also brings up the font subsystem), so it must
     * be called once before any rendering. The framebuffer is cleared to opaque black.
     *
     * @param[in] fb_w  Framebuffer width in pixels (> 0).
     * @param[in] fb_h  Framebuffer height in pixels (> 0).
     *
     * @return true on success; false on invalid size or allocation failure.
     */
    bool er_software_backend_init(int fb_w, int fb_h);

    /**
     * @brief Frees the framebuffer and tears down the backend.
     *
     * After this call the framebuffer pointer returned by er_software_framebuffer() is invalid.
     */
    void er_software_backend_destroy(void);

    /**
     * @brief Resizes the framebuffer in place (for a runtime board-size change).
     *
     * Reallocates the framebuffer to @p fb_w x @p fb_h and clears it to opaque black. The backend stays
     * registered (the context pointer is unchanged), so the engine keeps rendering through it. Call
     * er_reset() afterwards before rebuilding the scene at the new size. Must be called after a successful
     * er_software_backend_init().
     *
     * @param[in] fb_w  New framebuffer width in pixels (> 0).
     * @param[in] fb_h  New framebuffer height in pixels (> 0).
     *
     * @return true on success; false on invalid size or allocation failure (the old framebuffer is kept).
     */
    bool er_software_backend_resize(int fb_w, int fb_h);

    /**
     * @brief Clears the entire framebuffer to a solid straight-alpha ARGB8888 color.
     *
     * @param[in] argb  Fill color (the alpha is stored as-is; pass 0xFF000000 for opaque black).
     */
    void er_software_clear(uint32_t argb);

    /**
     * @brief Returns the ARGB8888 framebuffer (row-major, no padding; 0xAARRGGBB per pixel).
     *
     * @return Pointer to fb_w * fb_h uint32_t pixels, or NULL before init / after destroy.
     */
    uint32_t* er_software_framebuffer(void);

    /**
     * @brief Returns the framebuffer width in pixels (0 before init).
     */
    int er_software_fb_width(void);

    /**
     * @brief Returns the framebuffer height in pixels (0 before init).
     */
    int er_software_fb_height(void);

#ifdef __cplusplus
}
#endif

#endif
