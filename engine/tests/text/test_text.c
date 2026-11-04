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

#include "er_scene.h"
#include "native_renderer.h"
#include "text_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FRAMEBUFFER_WIDTH 320
#define FRAMEBUFFER_HEIGHT 240

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Accumulates statistics from fill_rect / copy_rect callbacks during a render pass.
 */
typedef struct
{
    int pixels_drawn;  /**< Total pixel area covered by in-bounds draw calls. */
    int draw_count;    /**< Total number of draw calls received. */
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
 * @param[out] test_context  TestCtx to reset.
 */
static void ctx_reset(TestCtx* test_context)
{
    memset(test_context, 0, sizeof(*test_context));
    test_context->min_x = FRAMEBUFFER_WIDTH;
    test_context->max_x = 0;
    test_context->min_y = FRAMEBUFFER_HEIGHT;
    test_context->max_y = 0;
}

/**
 * @brief Backend fill_rect callback that records pixel statistics into a TestCtx.
 *
 * @param[in] argb    ARGB8888 fill color (unused).
 * @param[in] x       Left edge of the fill rectangle.
 * @param[in] y       Top edge of the fill rectangle.
 * @param[in] width   Width of the fill rectangle in pixels.
 * @param[in] height  Height of the fill rectangle in pixels.
 * @param[in] ctx     Pointer to the TestCtx to update.
 */
static void fill_cb(uint32_t argb, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    (void)argb;
    test_context->draw_count++;
    if (x < 0 || y < 0 || x + width > FRAMEBUFFER_WIDTH || y + height > FRAMEBUFFER_HEIGHT)
        test_context->out_of_bounds++;
    else
    {
        test_context->pixels_drawn += width * height;
        if (x < test_context->min_x)
            test_context->min_x = x;
        if (x + width > test_context->max_x)
            test_context->max_x = x + width;
        if (y < test_context->min_y)
            test_context->min_y = y;
        if (y + height > test_context->max_y)
            test_context->max_y = y + height;
    }
}

/**
 * @brief Backend copy_rect callback that records statistics into a TestCtx.
 *
 * @param[in] source  Source pixel buffer (unused).
 * @param[in] stride  Source stride in bytes (unused).
 * @param[in] x       Destination X coordinate.
 * @param[in] y       Destination Y coordinate.
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 * @param[in] ctx     Opaque context.
 */
static void copy_cb(const void* source, int stride, int x, int y, int width, int height, void* ctx)
{
    TestCtx* test_context = ctx;
    (void)source;
    (void)stride;
    test_context->draw_count++;
    if (x < 0 || y < 0 || x + width > FRAMEBUFFER_WIDTH || y + height > FRAMEBUFFER_HEIGHT)
        test_context->out_of_bounds++;
    else
    {
        test_context->pixels_drawn += width * height;
        if (x < test_context->min_x)
            test_context->min_x = x;
        if (x + width > test_context->max_x)
            test_context->max_x = x + width;
        if (y < test_context->min_y)
            test_context->min_y = y;
        if (y + height > test_context->max_y)
            test_context->max_y = y + height;
    }
}

/**
 * @brief Backend blend_rect callback (no-op stub for testing).
 *
 * @param[in] source  Source pixel buffer (unused).
 * @param[in] stride  Source stride in bytes (unused).
 * @param[in] alpha   Global alpha (unused).
 * @param[in] x       Destination X coordinate (unused).
 * @param[in] y       Destination Y coordinate (unused).
 * @param[in] width   Width in pixels (unused).
 * @param[in] height  Height in pixels (unused).
 * @param[in] ctx     Opaque context (unused).
 */
static void blend_cb(const void* source, int stride, uint8_t alpha, int x, int y, int width, int height, void* ctx)
{
    (void)source;
    (void)stride;
    (void)alpha;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)ctx;
}

/**
 * @brief Prints a failure message to stderr and returns EXIT_FAILURE.
 *
 * @param[in] message  Human-readable description of the failed assertion.
 *
 * @return EXIT_FAILURE.
 */
static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
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
    int width = 0, height = 0;
    er_text_measure("Hello", 14, NULL, 0, 0, &width, &height);
    if (width <= 0 || height <= 0)
        return fail("measure produced non-positive size");

    int double_width = 0, double_height = 0;
    er_text_measure("HelloHello", 14, NULL, 0, 0, &double_width, &double_height);
    if (double_width <= width)
        return fail("longer string did not measure wider");
    if (double_height != height)
        return fail("line height changed between measures");

    /* letter_spacing widens a non-empty string. */
    int unspaced_width = 0, spaced_width = 0, dummy = 0;
    er_text_measure("Hello", 14, NULL, 0, 0, &unspaced_width, &dummy);
    er_text_measure("Hello", 14, NULL, 2, 0, &spaced_width, &dummy);
    if (spaced_width <= unspaced_width)
        return fail("letter_spacing=2 did not produce wider measure");

    /* Bold measures wider than normal: the synthetic faux-bold adds 1px per glyph, which the
       renderer applies to the cursor advance.  "Hello" (5 glyphs) must measure exactly +5. */
    int bold_width = 0;
    er_text_measure("Hello", 14, NULL, 0, 1, &bold_width, &dummy);
    if (bold_width != unspaced_width + 5)
        return fail("bold measure did not add 1px per glyph");

    /* Span measurement matches: a single inherit-everything span equals the plain measure; a bold
       span equals the bold measure — so an auto-sized Text fits its styled runs without clipping. */
    ERTextSpan span_plain = {0};
    span_plain.font_weight = 0xFFU;
    span_plain.font_style = 0xFFU;
    span_plain.text_decoration = 0xFFU;
    span_plain.letter_spacing = ER_LAYOUT_AUTO;
    memcpy(span_plain.text, "Hello", 6);
    int span_width = 0;
    er_text_measure_spans(&span_plain, 1, 14, NULL, 0, 0, &span_width, &dummy);
    if (span_width != unspaced_width)
        return fail("inherit-everything span did not match the plain measure");

    ERTextSpan span_bold = span_plain;
    span_bold.font_weight = 1U;
    int bold_span_width = 0;
    er_text_measure_spans(&span_bold, 1, 14, NULL, 0, 0, &bold_span_width, &dummy);
    if (bold_span_width != bold_width)
        return fail("bold span did not match the bold plain measure");

    /* Two spans concatenate: "Hel" + "lo" must equal "Hello". */
    ERTextSpan two[2] = {span_plain, span_plain};
    memcpy(two[0].text, "Hel", 4);
    memcpy(two[1].text, "lo", 3);
    int two_span_width = 0;
    er_text_measure_spans(two, 2, 14, NULL, 0, 0, &two_span_width, &dummy);
    if (two_span_width != unspaced_width)
        return fail("two spans did not measure the same as the joined string");

    /* Line measurement breaks the way the renderer does: on word boundaries at the width, at explicit
       newlines, at most number_of_lines lines, and reports the widest line without trailing spaces. */
    {
        int hello_width = 0, world_width = 0;
        er_text_measure("Hello", 14, NULL, 0, 0, &hello_width, &dummy);
        er_text_measure("world", 14, NULL, 0, 0, &world_width, &dummy);
        const int widest_width = (hello_width > world_width) ? hello_width : world_width;
        ERTextRenderParams line_params;
        memset(&line_params, 0, sizeof(line_params));
        line_params.text = "Hello world";
        line_params.font_size = 14;
        int line_width = 0, line_count = 0;
        er_text_measure_lines(&line_params, 0, &line_width, &line_count);
        if (line_count != 1)
            return fail("measure_lines: an unbounded single line did not measure as one line");
        er_text_measure_lines(&line_params, widest_width, &line_width, &line_count);
        if (line_count != 2 || line_width != widest_width)
            return fail("measure_lines: two words at the wider word's width did not break onto two lines");
        line_params.number_of_lines = 1;
        er_text_measure_lines(&line_params, widest_width, &line_width, &line_count);
        if (line_count != 1 || line_width != hello_width)
            return fail("measure_lines: number_of_lines=1 did not stop after the first line");
        line_params.number_of_lines = 0;
        line_params.text = "Hello\nworld";
        er_text_measure_lines(&line_params, 0, &line_width, &line_count);
        if (line_count != 2 || line_width != widest_width)
            return fail("measure_lines: an explicit newline did not start a second line");
        line_params.text = "";
        er_text_measure_lines(&line_params, 100, &line_width, &line_count);
        if (line_count != 0 || line_width != 0)
            return fail("measure_lines: empty text should measure no lines");

        /* Spans break as one run: "Hello " + "world" breaks exactly like "Hello world". */
        ERTextSpan words[2] = {span_plain, span_plain};
        memcpy(words[0].text, "Hello ", 7);
        memcpy(words[1].text, "world", 6);
        line_params.text = NULL;
        line_params.spans = words;
        line_params.span_count = 2;
        er_text_measure_lines(&line_params, widest_width, &line_width, &line_count);
        if (line_count != 2 || line_width != widest_width)
            return fail("measure_lines: spans did not break the way the joined string does");
    }

    /* UTF-8: 2-byte sequence measures wider than the plain ASCII baseline. */
    int plain_width = 0, symbol_width = 0;
    er_text_measure("23C", 16, NULL, 0, 0, &plain_width, &dummy);
    er_text_measure("23\xC2\xB0"
                    "C",
                    16,
                    NULL,
                    0,
                    0,
                    &symbol_width,
                    &dummy);
    if (symbol_width <= plain_width)
        return fail("U+00B0 degree symbol did not measure wider than 2-char baseline");

    /* UTF-8: 3-byte sequence. */
    int ascii_width = 0, arrow_width = 0;
    er_text_measure("X", 16, NULL, 0, 0, &ascii_width, &dummy);
    er_text_measure("\xE2\x86\x92", 16, NULL, 0, 0, &arrow_width, &dummy);
    if (arrow_width <= 0)
        return fail("U+2192 right-arrow measured zero width");

    /* UTF-8: 4-byte sequence falls back to '?'. */
    int question_width = 0, emoji_width = 0;
    er_text_measure("?", 16, NULL, 0, 0, &question_width, &dummy);
    er_text_measure("\xF0\x9F\x98\x80", 16, NULL, 0, 0, &emoji_width, &dummy);
    if (emoji_width != question_width)
        return fail("4-byte UTF-8 (emoji) did not fall back to '?'");

    /* ---- Backend setup ---- */
    TestCtx test_context;
    ctx_reset(&test_context);
    EmbeddedRenderBackend backend = {fill_cb, copy_cb, blend_cb, NULL, NULL, &test_context};
    embedded_renderer_set_backend(&backend);

    /* ---- Basic render produces in-bounds pixels ---- */
    ERTextRenderParams params;
    memset(&params, 0, sizeof(params));
    params.text = "Hello";
    params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;

    er_text_render(&params);
    if (test_context.draw_count == 0)
        return fail("no draw calls emitted for basic render");
    if (test_context.pixels_drawn == 0)
        return fail("no pixels reported drawn for basic render");
    if (test_context.out_of_bounds != 0)
        return fail("fill_rect emitted outside framebuffer bounds for basic render");

    /* ---- Narrow clip: no out-of-bounds fills ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.clip = (ERRect){0, 0, 4, FRAMEBUFFER_HEIGHT};
    er_text_render(&params);
    if (test_context.out_of_bounds != 0)
        return fail("narrow clip emitted out-of-bounds fills");

    /* ---- textAlign LEFT vs CENTER: center must be shifted right ---- */
    params.clip = (ERRect){0, 0, 200, 40};

    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.text_align = ER_TEXT_ALIGN_LEFT;
    er_text_render(&params);
    const int left_min_x = test_context.min_x;

    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.text_align = ER_TEXT_ALIGN_CENTER;
    er_text_render(&params);
    const int center_min_x = test_context.min_x;

    if (center_min_x <= left_min_x)
        return fail("CENTER alignment did not shift text right compared to LEFT");

    /* ---- textAlign RIGHT must be further right than CENTER ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.text_align = ER_TEXT_ALIGN_RIGHT;
    er_text_render(&params);
    const int right_min_x = test_context.min_x;

    if (right_min_x <= center_min_x)
        return fail("RIGHT alignment did not shift text right compared to CENTER");

    /* ---- numberOfLines=1 + TAIL ellipsis: all pixels must stay within clip ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "This is a long string that should get truncated with an ellipsis";
    params.clip = (ERRect){0, 0, 120, 40};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    params.text_align = ER_TEXT_ALIGN_LEFT;
    params.number_of_lines = 1;
    params.ellipsize_mode = ER_TEXT_ELLIPSIZE_TAIL;
    er_text_render(&params);
    if (test_context.out_of_bounds != 0)
        return fail("ellipsis render emitted out-of-bounds pixels");
    if (test_context.draw_count == 0)
        return fail("ellipsis render produced no draw calls");

    /* ---- Word-boundary wrapping: wide text in narrow clip spans more lines ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "Hello World How Are You Today";
    params.clip = (ERRect){0, 0, 80, 200};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    er_text_render(&params);
    const int wrap_max_y = test_context.max_y;
    const int wrap_draw_count = test_context.draw_count;

    /* Single-line render of the same text (unlimited width) for comparison. */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, 200};
    er_text_render(&params);
    const int single_max_y = test_context.max_y;

    if (wrap_max_y <= single_max_y)
        return fail("word-wrapped text did not extend further down than single-line");
    if (wrap_draw_count == 0)
        return fail("wrapped render produced no draw calls");
    if (test_context.out_of_bounds != 0)
        return fail("wrapped render emitted out-of-bounds pixels");

    /* ---- letter_spacing during render stays in bounds ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "Spaced";
    params.clip = (ERRect){10, 5, 200, 40};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    params.letter_spacing = 4;
    er_text_render(&params);
    if (test_context.out_of_bounds != 0)
        return fail("letter_spacing render emitted out-of-bounds pixels");
    if (test_context.draw_count == 0)
        return fail("letter_spacing render produced no draw calls");

    /* ---- textDecoration UNDERLINE produces extra pixels below baseline ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "Underline";
    params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    params.text_decoration = ER_TEXT_DECORATION_UNDERLINE;
    er_text_render(&params);
    const int under_max_y = test_context.max_y;

    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.text_decoration = ER_TEXT_DECORATION_NONE;
    er_text_render(&params);
    const int plain_max_y = test_context.max_y;

    if (under_max_y <= plain_max_y)
        return fail("UNDERLINE did not extend pixel extent below plain text");

    /* ---- Regression: text after a space must render, not be trimmed ----
     * Earlier the line-break logic treated "saw any whitespace on this line"
     * as if the line ended in whitespace. "Hello world" was committed as just
     * "Hello", so typing past a space in a TextInput appeared to do nothing. */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "Hello world";
    params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    params.number_of_lines = 1;
    params.ellipsize_mode = ER_TEXT_ELLIPSIZE_CLIP;
    er_text_render(&params);
    const int two_word_max_x = test_context.max_x;

    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.text = "Hello";
    er_text_render(&params);
    const int one_word_max_x = test_context.max_x;

    if (two_word_max_x <= one_word_max_x)
        return fail("\"Hello world\" must render wider than \"Hello\" (post-space text was trimmed)");

    /* ---- Regression: trailing whitespace should still be trimmed at EOF ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.text = "Hello   ";
    er_text_render(&params);
    const int trailing_space_max_x = test_context.max_x;

    if (trailing_space_max_x > one_word_max_x + 2)
        return fail("trailing whitespace was not trimmed at end-of-string");

    /* ---- fontWeight bold: bold text must render wider than normal ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "Bold";
    params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    params.font_weight = 1;
    er_text_render(&params);
    const int bold_max_x = test_context.max_x;
    if (test_context.draw_count == 0)
        return fail("bold render produced no draw calls");
    if (test_context.out_of_bounds != 0)
        return fail("bold render emitted out-of-bounds pixels");

    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.font_weight = 0;
    er_text_render(&params);
    const int normal_max_x = test_context.max_x;

    if (bold_max_x <= normal_max_x)
        return fail("bold text did not render wider than normal text");

    /* ---- fontStyle italic: italic must render in-bounds ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    memset(&params, 0, sizeof(params));
    params.text = "Italic";
    params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
    params.color = 0xFFFFFFFFU;
    params.font_size = 14;
    params.font_style = 1;
    er_text_render(&params);
    if (test_context.draw_count == 0)
        return fail("italic render produced no draw calls");
    if (test_context.out_of_bounds != 0)
        return fail("italic render emitted out-of-bounds pixels");

    /* ---- bold-italic: combined must also render in-bounds ---- */
    ctx_reset(&test_context);
    backend.ctx = &test_context;
    embedded_renderer_set_backend(&backend);
    params.font_weight = 1;
    params.font_style = 1;
    er_text_render(&params);
    if (test_context.draw_count == 0)
        return fail("bold-italic render produced no draw calls");
    if (test_context.out_of_bounds != 0)
        return fail("bold-italic render emitted out-of-bounds pixels");

    /* ---- Nested spans: two spans render the same total pixels as the merged plain string ---- */
    {
        /* Render the merged string in plain mode. */
        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        memset(&params, 0, sizeof(params));
        params.text = "HelloWorld";
        params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
        params.color = 0xFFFFFFFFU;
        params.font_size = 14;
        er_text_render(&params);
        const int plain_pixels = test_context.pixels_drawn;
        const int plain_draw_count = test_context.draw_count;
        if (plain_draw_count == 0)
            return fail("span baseline: plain render produced no draw calls");

        /* Render the same content via two spans — no style overrides (all inherit). */
        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);

        ERTextSpan plain_spans[2];
        memset(plain_spans, 0, sizeof(plain_spans));
        /* Both spans inherit all style fields from the base params. */
        plain_spans[0].font_weight = 0xFFU;
        plain_spans[0].font_style = 0xFFU;
        plain_spans[0].text_decoration = 0xFFU;
        plain_spans[0].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(plain_spans[0].text, "Hello", ER_SPAN_TEXT_MAX);

        plain_spans[1].font_weight = 0xFFU;
        plain_spans[1].font_style = 0xFFU;
        plain_spans[1].text_decoration = 0xFFU;
        plain_spans[1].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(plain_spans[1].text, "World", ER_SPAN_TEXT_MAX);

        memset(&params, 0, sizeof(params));
        params.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
        params.color = 0xFFFFFFFFU;
        params.font_size = 14;
        params.span_count = 2;
        params.spans = plain_spans;
        er_text_render(&params);

        if (test_context.draw_count == 0)
            return fail("span render produced no draw calls");
        if (test_context.out_of_bounds != 0)
            return fail("span render emitted out-of-bounds pixels");
        if (test_context.pixels_drawn != plain_pixels)
            return fail("two inherit-all spans did not match plain render pixel count");
    }

    /* ---- Span color override: second span must land at the same x as first-span end ---- */
    {
        /* Single-span in red produces some pixels. */
        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);

        ERTextSpan color_spans[2];
        memset(color_spans, 0, sizeof(color_spans));

        color_spans[0].font_weight = 0xFFU;
        color_spans[0].font_style = 0xFFU;
        color_spans[0].text_decoration = 0xFFU;
        color_spans[0].letter_spacing = ER_LAYOUT_AUTO;
        color_spans[0].color = 0xFFFF0000U; /* red */
        strncpy(color_spans[0].text, "AB", ER_SPAN_TEXT_MAX);

        color_spans[1].font_weight = 0xFFU;
        color_spans[1].font_style = 0xFFU;
        color_spans[1].text_decoration = 0xFFU;
        color_spans[1].letter_spacing = ER_LAYOUT_AUTO;
        color_spans[1].color = 0xFF0000FFU; /* blue */
        strncpy(color_spans[1].text, "CD", ER_SPAN_TEXT_MAX);

        ERTextRenderParams par2;
        memset(&par2, 0, sizeof(par2));
        par2.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
        par2.color = 0xFFFFFFFFU; /* base color — ignored because both spans override */
        par2.font_size = 14;
        par2.span_count = 2;
        par2.spans = color_spans;
        er_text_render(&par2);

        if (test_context.draw_count == 0)
            return fail("color-override span render produced no draw calls");
        if (test_context.out_of_bounds != 0)
            return fail("color-override span render emitted out-of-bounds pixels");
        if (test_context.max_x <= 0)
            return fail("color-override span render produced no x extent");
    }

    /* ---- Bold span: mixed-weight spans render in-bounds ---- */
    {
        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);

        ERTextSpan bold_spans[2];
        memset(bold_spans, 0, sizeof(bold_spans));

        bold_spans[0].font_weight = 0U; /* normal */
        bold_spans[0].font_style = 0xFFU;
        bold_spans[0].text_decoration = 0xFFU;
        bold_spans[0].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(bold_spans[0].text, "Normal", ER_SPAN_TEXT_MAX);

        bold_spans[1].font_weight = 1U; /* bold */
        bold_spans[1].font_style = 0xFFU;
        bold_spans[1].text_decoration = 0xFFU;
        bold_spans[1].letter_spacing = ER_LAYOUT_AUTO;
        strncpy(bold_spans[1].text, "Bold", ER_SPAN_TEXT_MAX);

        ERTextRenderParams par3;
        memset(&par3, 0, sizeof(par3));
        par3.clip = (ERRect){0, 0, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT};
        par3.color = 0xFFFFFFFFU;
        par3.font_size = 14;
        par3.span_count = 2;
        par3.spans = bold_spans;
        er_text_render(&par3);

        if (test_context.draw_count == 0)
            return fail("mixed-weight span render produced no draw calls");
        if (test_context.out_of_bounds != 0)
            return fail("mixed-weight span render emitted out-of-bounds pixels");
    }

    /* ---- Bold breaks at its drawn advance: a box exactly as wide as the regular text wraps bold ---- */
    {
        int regular_width = 0, line_height = 0;
        er_text_measure("Hello World", 14, NULL, 0, 0, &regular_width, &line_height);

        memset(&params, 0, sizeof(params));
        params.text = "Hello World";
        params.clip = (ERRect){0, 0, regular_width, FRAMEBUFFER_HEIGHT};
        params.color = 0xFFFFFFFFU;
        params.font_size = 14;
        params.number_of_lines = 2;

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        er_text_render(&params);
        if (test_context.max_y > line_height)
            return fail("regular text exactly as wide as its box did not fit on one line");

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        params.font_weight = 1;
        er_text_render(&params);
        if (test_context.max_y <= line_height)
            return fail("bold text wider than its box was not wrapped onto a second line");

        /* A bold span in a regular parent breaks at the bold advance too. */
        ERTextSpan bold_span = {0};
        bold_span.font_weight = 1U;
        bold_span.font_style = 0xFFU;
        bold_span.text_decoration = 0xFFU;
        bold_span.letter_spacing = ER_LAYOUT_AUTO;
        strncpy(bold_span.text, "Hello World", ER_SPAN_TEXT_MAX);

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        params.font_weight = 0;
        params.text = NULL;
        params.span_count = 1;
        params.spans = &bold_span;
        er_text_render(&params);
        if (test_context.max_y <= line_height)
            return fail("bold span wider than its box was not wrapped onto a second line");
    }

    /* ---- A letter-spaced span breaks and aligns at its own spacing, not the parent's ---- */
    {
        int regular_width = 0, line_height = 0;
        er_text_measure("Hello World", 14, NULL, 0, 0, &regular_width, &line_height);

        /* "Hello " inherits the parent's (zero) spacing; "World" spaces its glyphs 2px apart. */
        ERTextSpan spaced_spans[2];
        memset(spaced_spans, 0, sizeof(spaced_spans));
        for (int span_index = 0; span_index < 2; span_index++)
        {
            spaced_spans[span_index].font_weight = 0xFFU;
            spaced_spans[span_index].font_style = 0xFFU;
            spaced_spans[span_index].text_decoration = 0xFFU;
            spaced_spans[span_index].letter_spacing = ER_LAYOUT_AUTO;
        }
        strncpy(spaced_spans[0].text, "Hello ", ER_SPAN_TEXT_MAX);
        strncpy(spaced_spans[1].text, "World", ER_SPAN_TEXT_MAX);
        spaced_spans[1].letter_spacing = 2;

        int spaced_spans_width = 0, unused_height = 0;
        er_text_measure_spans(spaced_spans, 2, 14, NULL, 0, 0, &spaced_spans_width, &unused_height);
        if (spaced_spans_width <= regular_width)
            return fail("letter-spaced span did not measure wider than the unspaced text");

        /* A box as wide as the unspaced text wraps the spaced span onto a second line. */
        memset(&params, 0, sizeof(params));
        params.clip = (ERRect){0, 0, regular_width, FRAMEBUFFER_HEIGHT};
        params.color = 0xFFFFFFFFU;
        params.font_size = 14;
        params.number_of_lines = 2;
        params.span_count = 2;
        params.spans = spaced_spans;

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        er_text_render(&params);
        if (test_context.out_of_bounds != 0)
            return fail("letter-spaced span render emitted out-of-bounds pixels");
        if (test_context.max_y <= line_height)
            return fail("letter-spaced span wider than its box was not wrapped onto a second line");

        /* In a wide box, right alignment moves the line by exactly the box width minus its drawn width,
           and center alignment by half of that. */
        params.clip = (ERRect){0, 0, 200, FRAMEBUFFER_HEIGHT};
        params.number_of_lines = 0;

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        params.text_align = ER_TEXT_ALIGN_LEFT;
        er_text_render(&params);
        const int spaced_left_min_x = test_context.min_x;

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        params.text_align = ER_TEXT_ALIGN_RIGHT;
        er_text_render(&params);
        if (test_context.min_x - spaced_left_min_x != 200 - spaced_spans_width)
            return fail("right-aligned letter-spaced span was not placed at its drawn width");

        ctx_reset(&test_context);
        backend.ctx = &test_context;
        embedded_renderer_set_backend(&backend);
        params.text_align = ER_TEXT_ALIGN_CENTER;
        er_text_render(&params);
        if (test_context.min_x - spaced_left_min_x != (200 - spaced_spans_width) / 2)
            return fail("centered letter-spaced span was not placed at its drawn width");
    }

    return EXIT_SUCCESS;
}
