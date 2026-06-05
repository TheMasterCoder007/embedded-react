#ifndef EMBEDDED_REACT_PERF_OVERLAY_H
#define EMBEDDED_REACT_PERF_OVERLAY_H

/*
 * Built-in performance overlay (LVGL-style). A small metrics panel — FPS, CPU load, free memory —
 * drawn in the bottom-right corner of the framebuffer, on top of the app.
 *
 * Toggle with the ER_PERF_OVERLAY preprocessor define: 0 = compiled out (zero cost), 1 = enabled.
 * Override it from the build (e.g. -DER_PERF_OVERLAY=1, or a component compile definition) or by
 * editing the default below. The metrics themselves are platform-specific, so the host gathers them
 * (frame timing, heap stats) and feeds formatted lines to er_perf_overlay_draw(); the engine only
 * renders the panel via its font + the active render backend.
 */
#ifndef ER_PERF_OVERLAY
#define ER_PERF_OVERLAY 0
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Draws the performance overlay panel in the bottom-right of the framebuffer.
     *
     * Renders a translucent box plus the given text lines using the engine's built-in font, blitting
     * through the active render backend. Call once per frame AFTER er_commit() (so it composites on
     * top of the app and the backend marks the region dirty for the next present). The lines are
     * caller-owned, formatted however the host likes (e.g. "FPS 50", "CPU 55%").
     *
     * Compiles to a no-op when ER_PERF_OVERLAY == 0.
     *
     * @param[in]  screen_w     Framebuffer width in pixels.
     * @param[in]  screen_h     Framebuffer height in pixels.
     * @param[in]  lines        Array of null-terminated text lines, drawn top to bottom.
     * @param[in]  line_count   Number of lines in @p lines.
     * @param[out] out_x        Receives the drawn panel's left edge (NULL to ignore).
     * @param[out] out_y        Receives the drawn panel's top edge (NULL to ignore).
     * @param[out] out_w        Receives the drawn panel's width (NULL to ignore).
     * @param[out] out_h        Receives the drawn panel's height (NULL to ignore). 0 when nothing was
     *                          drawn. The host can use this rect to snapshot the overlay and
     *                          re-composite it each frame without re-rendering the text.
     */
    void er_perf_overlay_draw(int screen_w,
                              int screen_h,
                              const char* const* lines,
                              int line_count,
                              int* out_x,
                              int* out_y,
                              int* out_w,
                              int* out_h);

#ifdef __cplusplus
}
#endif

#endif
