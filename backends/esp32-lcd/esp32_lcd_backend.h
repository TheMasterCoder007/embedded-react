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

#ifndef EMBEDDED_REACT_ESP32_LCD_BACKEND_H
#define EMBEDDED_REACT_ESP32_LCD_BACKEND_H

#include "esp_lcd_panel_ops.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialises the ESP32 LCD render backend and registers it with the engine.
     *
     * Board-agnostic: it keeps a persistent "canonical" framebuffer in PSRAM that the engine's
     * fill/copy/blend composite into, and on each frame flushes the changed (dirty) region to the
     * supplied esp_lcd panel via esp_lcd_panel_draw_bitmap. The canonical framebuffer's pixel format
     * is selectable with ER_LCD_FB_RGB565 (default 1): RGB565 makes present a plain 565→565 copy (no
     * per-frame ARGB→565 conversion, half the bandwidth/footprint); ARGB8888 (0) keeps a full-precision
     * buffer and converts at present. The caller owns the panel (init it first — pins, timings, reset,
     * backlight) and must keep it alive for the backend's lifetime.
     *
     * The size and rotation come from the engine: call er_set_display_rotation() with the panel's size
     * first. The canonical framebuffer has the logical shape, and present turns it onto the panel with
     * er_rotate_copy(). Direct mode needs rotation 0.
     *
     * @param[in] panel  An initialised esp_lcd panel handle (e.g. an RGB panel).
     *
     * @return true on success; false when no display size is set, the panel has no framebuffer, or an
     *         allocation fails.
     */
    bool er_esp32_lcd_backend_init(esp_lcd_panel_handle_t panel);

    /**
     * @brief Flushes the painted (dirty) region of the framebuffer to the panel.
     *
     * Call once per frame after er_commit(). The engine does not auto-present — it only paints into
     * the backend via fill/copy/blend; this pushes the result to the display. No-op if nothing
     * changed since the last present.
     */
    void er_esp32_lcd_present(void);

    /**
     * @brief Returns true when the PIE SIMD blend routines are active (ESP32-S3, RGB565
     *        canonical format, and the init-time self-test passed).
     */
    bool er_esp32_lcd_pie_enabled(void);

    /**
     * @brief Snapshots a rectangle of the framebuffer as the persistent "overlay" region.
     *
     * Captures the given rect (already composited in the framebuffer, e.g. the perf overlay) into an
     * internal cache. er_esp32_lcd_present() puts that cache back on top wherever the app draws over
     * it, and sends a fresh capture to every panel framebuffer, so a small always-on overlay can be
     * drawn (expensively) just a couple of times a second yet stay flicker-free over a double-buffered
     * panel — without enlarging the app's per-frame dirty region. Pass w<=0 or h<=0 to disable it.
     *
     * @param[in] x  Left edge of the overlay rect in framebuffer pixels.
     * @param[in] y  Top edge of the overlay rect.
     * @param[in] width  Width in pixels (<=0 disables the overlay).
     * @param[in] height  Height in pixels (<=0 disables the overlay).
     */
    void er_esp32_lcd_overlay_capture(int x, int y, int width, int height);

    /**
     * @brief Saves the dirty box before the host draws the overlay into the framebuffer.
     *
     * Call immediately before rendering the overlay (whose draw dirties its rect) and pair with
     * er_esp32_lcd_overlay_capture(), which restores the dirty box afterwards. This keeps the overlay
     * out of the app's per-frame dirty region (it is composited separately each present), so a small
     * corner overlay never balloons the app flush.
     */
    void er_esp32_lcd_overlay_begin(void);

#ifdef __cplusplus
}
#endif

#endif
