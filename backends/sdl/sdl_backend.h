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

#ifndef EMBEDDED_REACT_SDL_BACKEND_H
#define EMBEDDED_REACT_SDL_BACKEND_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /*----------------------------------------------------------------------------------------------------------------------
     - Functions: Public
     ---------------------------------------------------------------------------------------------------------------------*/

    /**
     * @brief Initialises the SDL2 render backend and registers it with the engine.
     *
     * Creates two internal textures sized to fb_w × fb_h:
     *   - A render-target (ARGB8888) that acts as a persistent framebuffer; all engine
     *     draw calls write into it, and er_sdl_present() blits it to the screen each frame
     *     regardless of whether er_commit() did any work.
     *   - A streaming (ARGB8888) scratch texture used by copy_rect and blend_rect.
     *
     * The caller retains ownership of renderer and is responsible for destroying it
     * after er_sdl_backend_destroy(). Requires SDL 2.0.6 or later
     * (SDL_ComposeCustomBlendMode).
     *
     * @param[in] renderer  SDL2 renderer to draw into. Must outlive the backend.
     * @param[in] fb_w      Framebuffer width in pixels (must be > 0).
     * @param[in] fb_h      Framebuffer height in pixels (must be > 0).
     *
     * @return true on success, false if either texture could not be created.
     */
    bool er_sdl_backend_init(SDL_Renderer* renderer, int fb_w, int fb_h);

    /**
     * @brief Composites the persistent framebuffer onto the screen and presents it.
     *
     * Blits the internal render-target texture to the SDL renderer, then calls
     * SDL_RenderPresent(). Safe to call even when er_commit() was a no-op — the last
     * rendered frame is preserved in the framebuffer texture between calls.
     */
    void er_sdl_present(void);

    /**
     * @brief Toggles a diagnostic overlay that outlines the engine's dirty rect each frame.
     *
     * When enabled, er_sdl_present() draws a yellow rectangle around the union of all
     * pixels repainted by the most recent er_commit().  Frames with no dirty pixels
     * produce no outline.  The overlay is drawn in the logical coordinate space of the
     * SDL window, so pass the same DPI scale factor that was used to compute the
     * physical framebuffer size.
     *
     * @param[in] enable  true to show the overlay, false to hide it.
     * @param[in] scale   DPI scale factor (physical pixels / logical pixels); pass 1.0
     *                    on non-HiDPI displays.
     */
    void er_sdl_set_show_dirty_rect(bool enable, float scale);

    /**
     * @brief Destroys the SDL2 backend and frees its scratch texture.
     *
     * The SDL_Renderer passed to er_sdl_backend_init() is not destroyed here;
     * the caller owns it.
     */
    void er_sdl_backend_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
