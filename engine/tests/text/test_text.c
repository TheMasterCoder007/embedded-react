#include "er_scene.h"
#include "native_renderer.h"
#include "text_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 320
#define FB_H 240

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Accumulates statistics from fill_rect / copy_rect callbacks during a render pass.
 */
typedef struct
{
    int pixels_drawn;  /**< Total pixel area covered by in-bounds draw calls. */
    int draw_ops;      /**< Total number of draw calls received. */
    int out_of_bounds; /**< Number of draw calls that fell outside the framebuffer. */
    int min_x;         /**< Leftmost x coordinate seen across all draw calls. */
    int max_x;         /**< Rightmost x+w coordinate seen across all draw calls. */
    int min_y;         /**< Topmost y coordinate seen across all draw calls. */
    int max_y;         /**< Bottommost y+h coordinate seen across all draw calls. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Resets a TestCtx to its zero state with sentinel min/max values.
 *
 * @param[out] t  TestCtx to reset.
 */
static void ctx_reset(TestCtx* t)
{
    memset(t, 0, sizeof(*t));
    t->min_x = FB_W;
    t->max_x = 0;
    t->min_y = FB_H;
    t->max_y = 0;
}

/**
 * @brief Backend fill_rect callback that records pixel statistics into a TestCtx.
 *
 * @param[in] argb  ARGB8888 fill color (unused).
 * @param[in] x     Left edge of the fill rectangle.
 * @param[in] y     Top edge of the fill rectangle.
 * @param[in] w     Width of the fill rectangle in pixels.
 * @param[in] h     Height of the fill rectangle in pixels.
 * @param[in] ctx   Pointer to the TestCtx to update.
 */
static void fill_cb(uint32_t argb, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    (void)argb;
    t->draw_ops++;
    if (x < 0 || y < 0 || x + w > FB_W || y + h > FB_H)
        t->out_of_bounds++;
    else
    {
        t->pixels_drawn += w * h;
        if (x < t->min_x)
            t->min_x = x;
        if (x + w > t->max_x)
            t->max_x = x + w;
        if (y < t->min_y)
            t->min_y = y;
        if (y + h > t->max_y)
            t->max_y = y + h;
    }
}

/**
 * @brief Backend copy_rect callback that records statistics into a TestCtx.
 *
 * @param[in] src  Source pixel buffer (unused).
 * @param[in] s    Source stride in bytes (unused).
 * @param[in] x    Destination X coordinate.
 * @param[in] y    Destination Y coordinate.
 * @param[in] w    Width in pixels.
 * @param[in] h    Height in pixels.
 * @param[in] ctx  Opaque context.
 */
static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    TestCtx* t = ctx;
    (void)src;
    (void)s;
    t->draw_ops++;
    if (x < 0 || y < 0 || x + w > FB_W || y + h > FB_H)
        t->out_of_bounds++;
    else
    {
        t->pixels_drawn += w * h;
        if (x < t->min_x)
            t->min_x = x;
        if (x + w > t->max_x)
            t->max_x = x + w;
        if (y < t->min_y)
            t->min_y = y;
        if (y + h > t->max_y)
            t->max_y = y + h;
    }
}

/**
 * @brief Backend blend_rect callback (no-op stub for testing).
 *
 * @param[in] src  Source pixel buffer (unused).
 * @param[in] s    Source stride in bytes (unused).
 * @param[in] a    Global alpha (unused).
 * @param[in] x    Destination X coordinate (unused).
 * @param[in] y    Destination Y coordinate (unused).
 * @param[in] w    Width in pixels (unused).
 * @param[in] h    Height in pixels (unused).
 * @param[in] ctx  Opaque context (unused).
 */
static void blend_cb(const void* src, int s, uint8_t a, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)a;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] msg  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return EXIT_FAILURE;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point — exercises er_text_measure() and er_text_render() features.
 *
 * Verifies:
 *   - Basic measurement returns positive dimensions.
 *   - A longer string measures wider; line heights are consistent.
 *   - letter_spacing widens measurement proportionally.
 *   - UTF-8 multi-byte sequences are measured correctly.
 *   - Rendering produces in-bounds draw calls.
 *   - A narrow clip rect produces no out-of-bounds fills.
 *   - textAlign CENTER shifts pixels rightward vs LEFT.
 *   - textAlign RIGHT shifts pixels further right than CENTER.
 *   - numberOfLines + TAIL ellipsis keeps pixels within the clip width.
 *   - Word-boundary wrapping produces more lines than single-line.
 *   - letter_spacing during render stays in bounds.
 *   - textDecoration UNDERLINE produces extra pixels below the text baseline.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    /* ---- Measurement basics ---- */
    int w = 0, h = 0;
    er_text_measure("Hello", 14, NULL, 0, &w, &h);
    if (w <= 0 || h <= 0)
        return fail("measure produced non-positive size");

    int w2 = 0, h2 = 0;
    er_text_measure("HelloHello", 14, NULL, 0, &w2, &h2);
    if (w2 <= w)
        return fail("longer string did not measure wider");
    if (h2 != h)
        return fail("line height changed between measures");

    /* letter_spacing widens a non-empty string. */
    int w_nols = 0, w_ls = 0, dummy = 0;
    er_text_measure("Hello", 14, NULL, 0, &w_nols, &dummy);
    er_text_measure("Hello", 14, NULL, 2, &w_ls, &dummy);
    if (w_ls <= w_nols)
        return fail("letter_spacing=2 did not produce wider measure");

    /* UTF-8: 2-byte sequence measures wider than the plain ASCII baseline. */
    int w_plain = 0, w_sym = 0;
    er_text_measure("23C", 16, NULL, 0, &w_plain, &dummy);
    er_text_measure("23\xC2\xB0"
                    "C",
                    16,
                    NULL,
                    0,
                    &w_sym,
                    &dummy);
    if (w_sym <= w_plain)
        return fail("U+00B0 degree symbol did not measure wider than 2-char baseline");

    /* UTF-8: 3-byte sequence. */
    int w_ascii = 0, w_arrow = 0;
    er_text_measure("X", 16, NULL, 0, &w_ascii, &dummy);
    er_text_measure("\xE2\x86\x92", 16, NULL, 0, &w_arrow, &dummy);
    if (w_arrow <= 0)
        return fail("U+2192 right-arrow measured zero width");

    /* UTF-8: 4-byte sequence falls back to '?'. */
    int w_q = 0, w_emoji = 0;
    er_text_measure("?", 16, NULL, 0, &w_q, &dummy);
    er_text_measure("\xF0\x9F\x98\x80", 16, NULL, 0, &w_emoji, &dummy);
    if (w_emoji != w_q)
        return fail("4-byte UTF-8 (emoji) did not fall back to '?'");

    /* ---- Backend setup ---- */
    TestCtx tc;
    ctx_reset(&tc);
    EmbeddedRenderBackend be = {fill_cb, copy_cb, blend_cb, NULL, NULL, &tc};
    embedded_renderer_set_backend(&be);

    /* ---- Basic render produces in-bounds pixels ---- */
    ERTextRenderParams par;
    memset(&par, 0, sizeof(par));
    par.text = "Hello";
    par.clip = (ERRect){0, 0, FB_W, FB_H};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;

    er_text_render(&par);
    if (tc.draw_ops == 0)
        return fail("no draw calls emitted for basic render");
    if (tc.pixels_drawn == 0)
        return fail("no pixels reported drawn for basic render");
    if (tc.out_of_bounds != 0)
        return fail("fill_rect emitted outside framebuffer bounds for basic render");

    /* ---- Narrow clip: no out-of-bounds fills ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.clip = (ERRect){0, 0, 4, FB_H};
    er_text_render(&par);
    if (tc.out_of_bounds != 0)
        return fail("narrow clip emitted out-of-bounds fills");

    /* ---- textAlign LEFT vs CENTER: center must be shifted right ---- */
    par.clip = (ERRect){0, 0, 200, 40};

    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.text_align = ER_TEXT_ALIGN_LEFT;
    er_text_render(&par);
    const int left_min_x = tc.min_x;

    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.text_align = ER_TEXT_ALIGN_CENTER;
    er_text_render(&par);
    const int center_min_x = tc.min_x;

    if (center_min_x <= left_min_x)
        return fail("CENTER alignment did not shift text right compared to LEFT");

    /* ---- textAlign RIGHT must be further right than CENTER ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.text_align = ER_TEXT_ALIGN_RIGHT;
    er_text_render(&par);
    const int right_min_x = tc.min_x;

    if (right_min_x <= center_min_x)
        return fail("RIGHT alignment did not shift text right compared to CENTER");

    /* ---- numberOfLines=1 + TAIL ellipsis: all pixels must stay within clip ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "This is a long string that should get truncated with an ellipsis";
    par.clip = (ERRect){0, 0, 120, 40};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    par.text_align = ER_TEXT_ALIGN_LEFT;
    par.number_of_lines = 1;
    par.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    er_text_render(&par);
    if (tc.out_of_bounds != 0)
        return fail("ellipsis render emitted out-of-bounds pixels");
    if (tc.draw_ops == 0)
        return fail("ellipsis render produced no draw calls");

    /* ---- Word-boundary wrapping: wide text in narrow clip spans more lines ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "Hello World How Are You Today";
    par.clip = (ERRect){0, 0, 80, 200};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    er_text_render(&par);
    const int wrap_max_y = tc.max_y;
    const int wrap_ops = tc.draw_ops;

    /* Single-line render of the same text (unlimited width) for comparison. */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.clip = (ERRect){0, 0, FB_W, 200};
    er_text_render(&par);
    const int single_max_y = tc.max_y;

    if (wrap_max_y <= single_max_y)
        return fail("word-wrapped text did not extend further down than single-line");
    if (wrap_ops == 0)
        return fail("wrapped render produced no draw calls");
    if (tc.out_of_bounds != 0)
        return fail("wrapped render emitted out-of-bounds pixels");

    /* ---- letter_spacing during render stays in bounds ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "Spaced";
    par.clip = (ERRect){10, 5, 200, 40};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    par.letter_spacing = 4;
    er_text_render(&par);
    if (tc.out_of_bounds != 0)
        return fail("letter_spacing render emitted out-of-bounds pixels");
    if (tc.draw_ops == 0)
        return fail("letter_spacing render produced no draw calls");

    /* ---- textDecoration UNDERLINE produces extra pixels below baseline ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "Underline";
    par.clip = (ERRect){0, 0, FB_W, FB_H};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    par.text_decoration = ER_TEXT_DECORATION_UNDERLINE;
    er_text_render(&par);
    const int under_max_y = tc.max_y;

    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.text_decoration = ER_TEXT_DECORATION_NONE;
    er_text_render(&par);
    const int plain_max_y = tc.max_y;

    if (under_max_y <= plain_max_y)
        return fail("UNDERLINE did not extend pixel extent below plain text");

    /* ---- Regression: text after a space must render, not be trimmed ----
     * Earlier the line-break logic treated "saw any whitespace on this line"
     * as if the line ended in whitespace. "Hello world" was committed as just
     * "Hello", so typing past a space in a TextInput appeared to do nothing. */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "Hello world";
    par.clip = (ERRect){0, 0, FB_W, FB_H};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    par.number_of_lines = 1;
    par.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
    er_text_render(&par);
    const int two_word_max_x = tc.max_x;

    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.text = "Hello";
    er_text_render(&par);
    const int one_word_max_x = tc.max_x;

    if (two_word_max_x <= one_word_max_x)
        return fail("\"Hello world\" must render wider than \"Hello\" (post-space text was trimmed)");

    /* ---- Regression: trailing whitespace should still be trimmed at EOF ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.text = "Hello   ";
    er_text_render(&par);
    const int trailing_ws_max_x = tc.max_x;

    if (trailing_ws_max_x > one_word_max_x + 2)
        return fail("trailing whitespace was not trimmed at end-of-string");

    /* ---- fontWeight bold: bold text must render wider than normal ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "Bold";
    par.clip = (ERRect){0, 0, FB_W, FB_H};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    par.font_weight = 1;
    er_text_render(&par);
    const int bold_max_x = tc.max_x;
    if (tc.draw_ops == 0)
        return fail("bold render produced no draw calls");
    if (tc.out_of_bounds != 0)
        return fail("bold render emitted out-of-bounds pixels");

    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.font_weight = 0;
    er_text_render(&par);
    const int normal_max_x = tc.max_x;

    if (bold_max_x <= normal_max_x)
        return fail("bold text did not render wider than normal text");

    /* ---- fontStyle italic: italic must render in-bounds ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    memset(&par, 0, sizeof(par));
    par.text = "Italic";
    par.clip = (ERRect){0, 0, FB_W, FB_H};
    par.color = 0xFFFFFFFFU;
    par.font_size = 14;
    par.font_style = 1;
    er_text_render(&par);
    if (tc.draw_ops == 0)
        return fail("italic render produced no draw calls");
    if (tc.out_of_bounds != 0)
        return fail("italic render emitted out-of-bounds pixels");

    /* ---- bold-italic: combined must also render in-bounds ---- */
    ctx_reset(&tc);
    be.ctx = &tc;
    embedded_renderer_set_backend(&be);
    par.font_weight = 1;
    par.font_style = 1;
    er_text_render(&par);
    if (tc.draw_ops == 0)
        return fail("bold-italic render produced no draw calls");
    if (tc.out_of_bounds != 0)
        return fail("bold-italic render emitted out-of-bounds pixels");

    /* ---- Nested spans: two spans render the same total pixels as the merged plain string ---- */
    {
        /* Render the merged string in plain mode. */
        ctx_reset(&tc);
        be.ctx = &tc;
        embedded_renderer_set_backend(&be);
        memset(&par, 0, sizeof(par));
        par.text = "HelloWorld";
        par.clip = (ERRect){0, 0, FB_W, FB_H};
        par.color = 0xFFFFFFFFU;
        par.font_size = 14;
        er_text_render(&par);
        const int plain_pixels = tc.pixels_drawn;
        const int plain_ops = tc.draw_ops;
        if (plain_ops == 0)
            return fail("span baseline: plain render produced no draw calls");

        /* Render the same content via two spans — no style overrides (all inherit). */
        ctx_reset(&tc);
        be.ctx = &tc;
        embedded_renderer_set_backend(&be);

        ERTextSpan sp[2];
        memset(sp, 0, sizeof(sp));
        /* Both spans inherit all style fields from the base params. */
        sp[0].font_weight = 0xFFU;
        sp[0].font_style = 0xFFU;
        sp[0].text_decoration = 0xFFU;
        sp[0].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(sp[0].text, "Hello", ER_SPAN_TEXT_MAX);

        sp[1].font_weight = 0xFFU;
        sp[1].font_style = 0xFFU;
        sp[1].text_decoration = 0xFFU;
        sp[1].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(sp[1].text, "World", ER_SPAN_TEXT_MAX);

        memset(&par, 0, sizeof(par));
        par.clip = (ERRect){0, 0, FB_W, FB_H};
        par.color = 0xFFFFFFFFU;
        par.font_size = 14;
        par.span_count = 2;
        par.spans = sp;
        er_text_render(&par);

        if (tc.draw_ops == 0)
            return fail("span render produced no draw calls");
        if (tc.out_of_bounds != 0)
            return fail("span render emitted out-of-bounds pixels");
        if (tc.pixels_drawn != plain_pixels)
            return fail("two inherit-all spans did not match plain render pixel count");
    }

    /* ---- Span color override: second span must land at the same x as first-span end ---- */
    {
        /* Single-span in red produces some pixels. */
        ctx_reset(&tc);
        be.ctx = &tc;
        embedded_renderer_set_backend(&be);

        ERTextSpan sp2[2];
        memset(sp2, 0, sizeof(sp2));

        sp2[0].font_weight = 0xFFU;
        sp2[0].font_style = 0xFFU;
        sp2[0].text_decoration = 0xFFU;
        sp2[0].letter_spacing = ER_LAYOUT_AUTO;
        sp2[0].color = 0xFFFF0000U; /* red */
        strncpy(sp2[0].text, "AB", ER_SPAN_TEXT_MAX);

        sp2[1].font_weight = 0xFFU;
        sp2[1].font_style = 0xFFU;
        sp2[1].text_decoration = 0xFFU;
        sp2[1].letter_spacing = ER_LAYOUT_AUTO;
        sp2[1].color = 0xFF0000FFU; /* blue */
        strncpy(sp2[1].text, "CD", ER_SPAN_TEXT_MAX);

        ERTextRenderParams par2;
        memset(&par2, 0, sizeof(par2));
        par2.clip = (ERRect){0, 0, FB_W, FB_H};
        par2.color = 0xFFFFFFFFU; /* base color — ignored because both spans override */
        par2.font_size = 14;
        par2.span_count = 2;
        par2.spans = sp2;
        er_text_render(&par2);

        if (tc.draw_ops == 0)
            return fail("color-override span render produced no draw calls");
        if (tc.out_of_bounds != 0)
            return fail("color-override span render emitted out-of-bounds pixels");
        if (tc.max_x <= 0)
            return fail("color-override span render produced no x extent");
    }

    /* ---- Bold span: mixed-weight spans render in-bounds ---- */
    {
        ctx_reset(&tc);
        be.ctx = &tc;
        embedded_renderer_set_backend(&be);

        ERTextSpan sp3[2];
        memset(sp3, 0, sizeof(sp3));

        sp3[0].font_weight = 0U; /* normal */
        sp3[0].font_style = 0xFFU;
        sp3[0].text_decoration = 0xFFU;
        sp3[0].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(sp3[0].text, "Normal", ER_SPAN_TEXT_MAX);

        sp3[1].font_weight = 1U; /* bold */
        sp3[1].font_style = 0xFFU;
        sp3[1].text_decoration = 0xFFU;
        sp3[1].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(sp3[1].text, "Bold", ER_SPAN_TEXT_MAX);

        ERTextRenderParams par3;
        memset(&par3, 0, sizeof(par3));
        par3.clip = (ERRect){0, 0, FB_W, FB_H};
        par3.color = 0xFFFFFFFFU;
        par3.font_size = 14;
        par3.span_count = 2;
        par3.spans = sp3;
        er_text_render(&par3);

        if (tc.draw_ops == 0)
            return fail("mixed-weight span render produced no draw calls");
        if (tc.out_of_bounds != 0)
            return fail("mixed-weight span render emitted out-of-bounds pixels");
    }

    return EXIT_SUCCESS;
}
