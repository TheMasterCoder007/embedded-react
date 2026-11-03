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

#include "perf_overlay.h"

#if ER_PERF_OVERLAY

#include "renderer_internal.h" /* er_blit_fill */
#include "text_renderer.h"     /* er_text_render / er_text_measure */

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_PERF_FONT_SIZE 16U /* metric text size in pixels */
#define ER_PERF_PADDING 6     /* inner padding around the text block */
#define ER_PERF_MARGIN 8      /* gap from the screen edges */
/* The panel background MUST be opaque: a redraw composites over the persistent framebuffer, so a
 * translucent box would let the previous text bleed through and leave a fading ghost. */
#define ER_PERF_BACKGROUND 0xFF141A24U /* opaque dark panel */
#define ER_PERF_FOREGROUND 0xFF7CF08CU /* light-green metric text */
/* Clamp so a stray count can't run away. Sized to fit a host's own metrics (FPS/CPU/heap) plus the
 * ER_PERF_OVERLAY_LINES frame-instrumentation lines from er_perf_overlay_lines(). */
#define ER_PERF_MAX_LINES 16

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_perf_overlay_draw(int screen_width,
                          int screen_height,
                          const char* const* lines,
                          int line_count,
                          int* out_x,
                          int* out_y,
                          int* out_width,
                          int* out_height)
{
    if (out_x)
        *out_x = 0;
    if (out_y)
        *out_y = 0;
    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;

    if (!lines || line_count <= 0 || screen_width <= 0 || screen_height <= 0)
    {
        return;
    }
    if (line_count > ER_PERF_MAX_LINES)
    {
        line_count = ER_PERF_MAX_LINES;
    }

    /* Measure the widest line and the font's natural line height. */
    int max_width = 0;
    int line_height = 0;
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        if (!lines[line_index])
        {
            continue;
        }
        int width = 0;
        int height = 0;
        er_text_measure(lines[line_index], (uint8_t)ER_PERF_FONT_SIZE, NULL, 0, 0, &width, &height);
        if (width > max_width)
        {
            max_width = width;
        }
        if (height > line_height)
        {
            line_height = height;
        }
    }
    if (max_width <= 0 || line_height <= 0)
    {
        return;
    }

    /* Grow-only width: the panel is anchored to a fixed right edge, so if it could shrink as a number
     * gets narrower (proportional font) the old, wider text would peek out past the new background.
     * Keeping the width monotonic (it stabilises within a couple of updates) keeps the box stationary
     * and the background always covering the previous frame's text. */
    static int s_sticky_width = 0;
    if (max_width > s_sticky_width)
    {
        s_sticky_width = max_width;
    }

    const int panel_width = s_sticky_width + ER_PERF_PADDING * 2;
    const int panel_height = line_height * line_count + ER_PERF_PADDING * 2;
    const int panel_x = screen_width - panel_width - ER_PERF_MARGIN;
    const int panel_y = screen_height - panel_height - ER_PERF_MARGIN;

    /* Report the drawn rect so the host can snapshot exactly this region (e.g. to re-composite the
     * overlay every frame without re-rendering the text). */
    if (out_x)
        *out_x = panel_x;
    if (out_y)
        *out_y = panel_y;
    if (out_width)
        *out_width = panel_width;
    if (out_height)
        *out_height = panel_height;

    /* Opaque background, composited over whatever the app drew. */
    er_blit_fill(ER_PERF_BACKGROUND, panel_x, panel_y, panel_width, panel_height);

    /* One render call per line so each sits on its own baseline. */
    for (int line_index = 0; line_index < line_count; line_index++)
    {
        if (!lines[line_index])
        {
            continue;
        }
        ERTextRenderParams params;
        memset(&params, 0, sizeof(params));
        params.text = lines[line_index];
        params.clip.x = panel_x + ER_PERF_PADDING;
        params.clip.y = panel_y + ER_PERF_PADDING + line_index * line_height;
        params.clip.w = panel_width - ER_PERF_PADDING * 2;
        params.clip.h = line_height;
        params.color = ER_PERF_FOREGROUND;
        params.font_size = (uint8_t)ER_PERF_FONT_SIZE;
        params.font_family = NULL;
        er_text_render(&params);
    }
}

#else /* ER_PERF_OVERLAY == 0 : compiled out */

void er_perf_overlay_draw(int screen_width,
                          int screen_height,
                          const char* const* lines,
                          int line_count,
                          int* out_x,
                          int* out_y,
                          int* out_width,
                          int* out_height)
{
    (void)screen_width;
    (void)screen_height;
    (void)lines;
    (void)line_count;
    if (out_x)
        *out_x = 0;
    if (out_y)
        *out_y = 0;
    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;
}

#endif
