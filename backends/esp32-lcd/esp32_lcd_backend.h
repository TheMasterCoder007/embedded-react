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
     * Board-agnostic: it keeps an ARGB8888 framebuffer in PSRAM that the engine's fill/copy/blend
     * composite into, and on each frame flushes the changed (dirty) region as RGB565 to the supplied
     * esp_lcd panel via esp_lcd_panel_draw_bitmap. The caller owns the panel (init it first — pins,
     * timings, reset, backlight) and must keep it alive for the backend's lifetime.
     *
     * @param[in] panel   An initialised esp_lcd panel handle (e.g. an RGB panel).
     * @param[in] width   Panel width in pixels (must match the engine's root width).
     * @param[in] height  Panel height in pixels.
     *
     * @return true on success; false if a framebuffer could not be allocated.
     */
    bool er_esp32_lcd_backend_init(esp_lcd_panel_handle_t panel, int width, int height);

    /**
     * @brief Flushes the painted (dirty) region of the framebuffer to the panel.
     *
     * Call once per frame after er_commit(). The engine does not auto-present — it only paints into
     * the backend via fill/copy/blend; this pushes the result to the display. No-op if nothing
     * changed since the last present.
     */
    void er_esp32_lcd_present(void);

    /**
     * @brief Snapshots a rectangle of the framebuffer as the persistent "overlay" region.
     *
     * Captures the given rect (already composited in the framebuffer, e.g. the perf overlay) into an
     * internal RGB565 cache. Every subsequent er_esp32_lcd_present() re-composites that cache on top
     * of the panel, so a small always-on overlay can be drawn (expensively) just a couple of times a
     * second yet stay flicker-free over a double-buffered panel — without enlarging the app's per-frame
     * dirty region. Pass w<=0 or h<=0 to disable re-compositing.
     *
     * @param[in] x  Left edge of the overlay rect in framebuffer pixels.
     * @param[in] y  Top edge of the overlay rect.
     * @param[in] w  Width in pixels (<=0 disables the overlay).
     * @param[in] h  Height in pixels (<=0 disables the overlay).
     */
    void er_esp32_lcd_overlay_capture(int x, int y, int w, int h);

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
