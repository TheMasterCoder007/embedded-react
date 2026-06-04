#include "perf_overlay.h"

#if ER_PERF_OVERLAY

#include "renderer_internal.h" /* er_blit_fill */
#include "text_renderer.h"     /* er_text_render / er_text_measure */

#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define ER_PERF_FONT_SIZE 16U /* metric text size in pixels */
#define ER_PERF_PAD 6         /* inner padding around the text block */
#define ER_PERF_MARGIN 8      /* gap from the screen edges */
/* The panel background MUST be opaque: a redraw composites over the persistent framebuffer, so a
 * translucent box would let the previous text bleed through and leave a fading ghost. */
#define ER_PERF_BG 0xFF141A24U /* opaque dark panel */
#define ER_PERF_FG 0xFF7CF08CU /* light-green metric text */
#define ER_PERF_MAX_LINES 8    /* clamp so a stray count can't run away */

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_perf_overlay_draw(int screen_w, int screen_h, const char* const* lines, int line_count)
{
    if (!lines || line_count <= 0 || screen_w <= 0 || screen_h <= 0)
    {
        return;
    }
    if (line_count > ER_PERF_MAX_LINES)
    {
        line_count = ER_PERF_MAX_LINES;
    }

    /* Measure the widest line and the font's natural line height. */
    int max_w = 0;
    int line_h = 0;
    for (int i = 0; i < line_count; i++)
    {
        if (!lines[i])
        {
            continue;
        }
        int w = 0;
        int h = 0;
        er_text_measure(lines[i], (uint8_t)ER_PERF_FONT_SIZE, NULL, 0, 0, &w, &h);
        if (w > max_w)
        {
            max_w = w;
        }
        if (h > line_h)
        {
            line_h = h;
        }
    }
    if (max_w <= 0 || line_h <= 0)
    {
        return;
    }

    /* Grow-only width: the panel is anchored to a fixed right edge, so if it could shrink as a number
     * gets narrower (proportional font) the old, wider text would peek out past the new background.
     * Keeping the width monotonic (it stabilises within a couple of updates) keeps the box stationary
     * and the background always covering the previous frame's text. */
    static int s_sticky_w = 0;
    if (max_w > s_sticky_w)
    {
        s_sticky_w = max_w;
    }

    const int panel_w = s_sticky_w + ER_PERF_PAD * 2;
    const int panel_h = line_h * line_count + ER_PERF_PAD * 2;
    const int panel_x = screen_w - panel_w - ER_PERF_MARGIN;
    const int panel_y = screen_h - panel_h - ER_PERF_MARGIN;

    /* Translucent background, composited over whatever the app drew. */
    er_blit_fill(ER_PERF_BG, panel_x, panel_y, panel_w, panel_h);

    /* One render call per line so each sits on its own baseline. */
    for (int i = 0; i < line_count; i++)
    {
        if (!lines[i])
        {
            continue;
        }
        ERTextRenderParams p;
        memset(&p, 0, sizeof(p));
        p.text = lines[i];
        p.clip.x = panel_x + ER_PERF_PAD;
        p.clip.y = panel_y + ER_PERF_PAD + i * line_h;
        p.clip.w = panel_w - ER_PERF_PAD * 2;
        p.clip.h = line_h;
        p.color = ER_PERF_FG;
        p.font_size = (uint8_t)ER_PERF_FONT_SIZE;
        p.font_family = NULL;
        er_text_render(&p);
    }
}

#else /* ER_PERF_OVERLAY == 0 : compiled out */

void er_perf_overlay_draw(int screen_w, int screen_h, const char* const* lines, int line_count)
{
    (void)screen_w;
    (void)screen_h;
    (void)lines;
    (void)line_count;
}

#endif
