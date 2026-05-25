#include "er_scene.h"
#include "native_renderer.h"
#include "text_renderer.h"
#include <stdio.h>
#include <stdlib.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

#define FB_W 256
#define FB_H 64

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Accumulates statistics from fill_rect callbacks during a render pass.
 */
typedef struct
{
    int pixels_drawn; /**< Total pixel area covered by in-bounds fill calls. */
    int fills;        /**< Total number of fill_rect calls received. */
    int out_of_bounds; /**< Number of fill_rect calls that fell outside the framebuffer. */
} TestCtx;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

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
    t->fills++;
    if (x < 0 || y < 0 || x + w > FB_W || y + h > FB_H)
        t->out_of_bounds++;
    else
        t->pixels_drawn += w * h;
}

/**
 * @brief Backend copy_rect callback (no-op stub for testing).
 *
 * @param[in] src  Source pixel buffer (unused).
 * @param[in] s    Source stride in bytes (unused).
 * @param[in] x    Destination X coordinate (unused).
 * @param[in] y    Destination Y coordinate (unused).
 * @param[in] w    Width in pixels (unused).
 * @param[in] h    Height in pixels (unused).
 * @param[in] ctx  Opaque context (unused).
 */
static void copy_cb(const void* src, int s, int x, int y, int w, int h, void* ctx)
{
    (void)src;
    (void)s;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)ctx;
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
 * @brief Test entry point — exercises er_text_measure() and er_text_render() behaviour.
 *
 * Verifies that:
 *   - Measurement returns positive dimensions for ASCII text.
 *   - A longer string measures wider.
 *   - fill_rect is called during rendering and produces pixels within bounds.
 *   - A narrow clip rect produces no out-of-bounds fills.
 *   - Multi-byte UTF-8 sequences (2-byte and 3-byte) measure correctly.
 *   - 4-byte UTF-8 sequences (emoji) fall back to the '?' glyph width.
 *
 * @return EXIT_SUCCESS on pass, EXIT_FAILURE on the first failed assertion.
 */
int main(void)
{
    int w = 0, h = 0;
    er_text_measure("Hello", 14, NULL, &w, &h);
    if (w <= 0 || h <= 0)
        return fail("measure produced non-positive size");

    int w2 = 0, h2 = 0;
    er_text_measure("HelloHello", 14, NULL, &w2, &h2);
    if (w2 <= w)
        return fail("longer string did not measure wider");
    if (h2 != h)
        return fail("line height changed between measures");

    TestCtx              tc = { 0 };
    EmbeddedRenderBackend be = { fill_cb, copy_cb, blend_cb, NULL, NULL, &tc };
    embedded_renderer_set_backend(&be);

    ERRect clip = { 0, 0, FB_W, FB_H };
    er_text_render("Hello", clip, 0xFFFFFFFFu, 14, NULL);

    if (tc.fills == 0)
        return fail("no fill_rect calls emitted");
    if (tc.pixels_drawn == 0)
        return fail("no pixels reported drawn");
    if (tc.out_of_bounds != 0)
        return fail("fill_rect emitted outside framebuffer bounds");

    TestCtx tc2 = { 0 };
    be.ctx = &tc2;
    embedded_renderer_set_backend(&be);
    ERRect tiny = { 0, 0, 4, 64 };
    er_text_render("Hello", tiny, 0xFFFFFFFFu, 14, NULL);
    if (tc2.out_of_bounds != 0)
        return fail("narrow clip emitted out-of-bounds fills");

    int w_plain = 0, w_sym = 0, dummy = 0;
    er_text_measure("23C", 16, NULL, &w_plain, &dummy);
    er_text_measure("23\xC2\xB0" "C", 16, NULL, &w_sym, &dummy);
    if (w_sym <= w_plain)
        return fail("U+00B0 degree symbol did not measure wider than 2-char baseline");

    int w_ascii = 0, w_arrow = 0;
    er_text_measure("X", 16, NULL, &w_ascii, &dummy);
    er_text_measure("\xE2\x86\x92", 16, NULL, &w_arrow, &dummy);
    if (w_arrow <= 0)
        return fail("U+2192 right-arrow measured zero width");

    int w_q = 0, w_emoji = 0;
    er_text_measure("?", 16, NULL, &w_q, &dummy);
    er_text_measure("\xF0\x9F\x98\x80", 16, NULL, &w_emoji, &dummy);
    if (w_emoji != w_q)
        return fail("4-byte UTF-8 (emoji) did not fall back to '?'");

    return EXIT_SUCCESS;
}
