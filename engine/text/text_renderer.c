#include "text_renderer.h"
#include "font_bitmap.h"
#include "font_registry.h"
#include "renderer_internal.h"
#include <limits.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Decodes the next UTF-8 codepoint from a string and advances the pointer.
 *
 * Supports 1-byte (ASCII), 2-byte, and 3-byte sequences. 4-byte sequences
 * (above U+FFFF) are consumed in full and return U+003F ('?'). Ill-formed bytes
 * advance by one and return U+003F ('?').
 *
 * @param[in,out] pp  Pointer to the current position in the UTF-8 string. Advanced
 *                    past the decoded sequence on return.
 *
 * @return The decoded Unicode codepoint, or U+003F on error.
 */
static uint32_t utf8_next(const char** pp)
{
    const uint8_t* p = (const uint8_t*)*pp;
    const uint8_t b0 = p[0];
    uint32_t cp = 0;
    int n = 0;
    int beyond_bmp = 0;

    if (b0 < 0x80U)
    {
        n = 1;
        cp = b0;
    }
    else if ((b0 & 0xE0U) == 0xC0U)
    {
        n = 2;
        cp = b0 & 0x1FU;
    }
    else if ((b0 & 0xF0U) == 0xE0U)
    {
        n = 3;
        cp = b0 & 0x0FU;
    }
    else if ((b0 & 0xF8U) == 0xF0U)
    {
        n = 4;
        beyond_bmp = 1;
    }
    else
    {
        *pp = (const char*)(p + 1);
        return '?';
    }

    for (int i = 1; i < n; i++)
    {
        const uint8_t b = p[i];
        if (b == 0)
        {
            *pp = (const char*)(p + i);
            return '?';
        }
        if ((b & 0xC0U) != 0x80U)
        {
            *pp = (const char*)(p + i);
            return '?';
        }
        if (!beyond_bmp)
            cp = cp << 6 | b & 0x3FU;
    }
    *pp = (const char*)(p + n);
    return beyond_bmp ? (uint32_t)'?' : cp;
}

/**
 * @brief Draws a single glyph bitmap into the framebuffer via run-length fill calls.
 *
 * Pixels are emitted as horizontal runs of set bits using er_blit_fill(). All
 * output is clipped to the supplied clip rectangle.
 *
 * @param[in] g         GlyphInfo describing the glyph dimensions and bitmap offset.
 * @param[in] bitmap    Pointer to the font's packed 1-bit-per-pixel bitmap data.
 * @param[in] cursor_x  Cursor X origin (left of the advance box) in framebuffer pixels.
 * @param[in] cursor_y  Cursor Y origin (top of the line box) in framebuffer pixels.
 * @param[in] clip      Clipping rectangle; no pixels are drawn outside this area.
 * @param[in] color     Text color as straight-alpha ARGB8888.
 */
static void
draw_glyph(const GlyphInfo* g, const uint8_t* bitmap, int cursor_x, int cursor_y, const ERRect* clip, uint32_t color)
{
    if (g->width == 0U || g->height == 0U)
        return;

    const uint8_t* bmp = bitmap + g->bitmap_offset;
    const int row_bytes = (g->width + 7) >> 3;

    const int clip_x1 = clip->x;
    const int clip_y1 = clip->y;
    const int clip_x2 = clip->x + clip->w;
    const int clip_y2 = clip->y + clip->h;

    const int origin_x = cursor_x + g->x_offset;
    const int origin_y = cursor_y + g->y_offset;

    for (int row = 0; row < g->height; row++)
    {
        const int fy = origin_y + row;
        if (fy < clip_y1 || fy >= clip_y2)
            continue;

        const uint8_t* src_row = bmp + (size_t)row * (size_t)row_bytes;
        int run_start = -1;

        for (int col = 0; col < g->width; col++)
        {
            const int fx = origin_x + col;
            const int in_clip_x = (fx >= clip_x1 && fx < clip_x2);
            const uint8_t bit = src_row[col >> 3] & (uint8_t)(0x80U >> (col & 7));

            if (bit && in_clip_x)
            {
                if (run_start < 0)
                    run_start = fx;
            }
            else if (run_start >= 0)
            {
                er_blit_fill(color, run_start, fy, fx - run_start, 1);
                run_start = -1;
            }
        }

        if (run_start >= 0)
        {
            const int run_end = origin_x + g->width;
            const int clipped_end = (run_end < clip_x2) ? run_end : clip_x2;
            er_blit_fill(color, run_start, fy, clipped_end - run_start, 1);
        }
    }
}

/**
 * @brief Draws a single anti-aliased glyph using grayscale coverage at any supported BPP (2, 4, or 8).
 *
 * Unpacks grayscale coverage values from the packed bitmap and scales them to 0-255:
 * 2-bit (0-3 × 85), 4-bit (0-15 × 17), 8-bit (identity). Builds a premultiplied
 * ARGB8888 row buffer and composites it via er_blit_copy.
 * Output is clipped to the supplied clip rectangle.
 *
 * @param[in] g         GlyphInfo describing the glyph dimensions and bitmap offset.
 * @param[in] bitmap    Pointer to the font's packed grayscale bitmap data.
 * @param[in] bpp       Bits per pixel of the font bitmap (2, 4, or 8).
 * @param[in] cursor_x  Cursor X origin (left of the advance box) in framebuffer pixels.
 * @param[in] cursor_y  Cursor Y origin (top of the line box) in framebuffer pixels.
 * @param[in] clip      Clipping rectangle; no pixels are drawn outside this area.
 * @param[in] color     Text color as straight-alpha ARGB8888.
 */
static void draw_glyph_aa(const GlyphInfo* g,
                          const uint8_t* bitmap,
                          uint8_t bpp,
                          int cursor_x,
                          int cursor_y,
                          const ERRect* clip,
                          uint32_t color)
{
    if (g->width == 0U || g->height == 0U)
        return;

    const uint8_t src_a = (uint8_t)(color >> 24);
    const uint8_t src_r = (uint8_t)(color >> 16);
    const uint8_t src_g = (uint8_t)(color >> 8);
    const uint8_t src_b = (uint8_t)(color);

    const uint8_t* bmp = bitmap + g->bitmap_offset;
    const int clip_x1 = clip->x;
    const int clip_y1 = clip->y;
    const int clip_x2 = clip->x + clip->w;
    const int clip_y2 = clip->y + clip->h;
    const int origin_x = cursor_x + g->x_offset;
    const int origin_y = cursor_y + g->y_offset;

    /* Row stride depends on BPP; glyph width is uint8_t so max is 255 px. */
    const int row_stride = (bpp == 8)   ? (int)g->width
                           : (bpp == 4) ? ((int)g->width + 1) / 2
                                        : ((int)g->width + 3) / 4; /* bpp == 2 */

    uint32_t row_buf[256]; /* stack buffer: max glyph width 255 px */

    for (int row = 0; row < (int)g->height; row++)
    {
        const int fy = origin_y + row;
        if (fy < clip_y1 || fy >= clip_y2)
            continue;

        const int col_start = (clip_x1 > origin_x) ? clip_x1 - origin_x : 0;
        const int col_end = (clip_x2 < origin_x + (int)g->width) ? clip_x2 - origin_x : (int)g->width;
        if (col_start >= col_end)
            continue;

        const uint8_t* src_row = bmp + (size_t)row * (size_t)row_stride;
        for (int col = col_start; col < col_end; col++)
        {
            uint8_t cov;
            if (bpp == 8)
            {
                cov = src_row[col];
            }
            else if (bpp == 4)
            {
                /* High nibble = left pixel of each pair. */
                const uint8_t nibble = (col & 1) ? src_row[col >> 1] & 0x0FU : src_row[col >> 1] >> 4;
                cov = nibble * 17U; /* 0-15 → 0,17,…,255 */
            }
            else
            {
                /* bpp == 2: bits 7-6 = leftmost pixel of each quartet. */
                const uint8_t pair = (src_row[col >> 2] >> (6U - ((uint8_t)col & 3U) * 2U)) & 0x03U;
                cov = pair * 85U; /* 0-3 → 0,85,170,255 */
            }

            const uint8_t a = (uint8_t)(((uint32_t)src_a * cov + 127U) / 255U);
            const uint8_t pr = (uint8_t)(((uint32_t)src_r * a + 127U) / 255U);
            const uint8_t pg = (uint8_t)(((uint32_t)src_g * a + 127U) / 255U);
            const uint8_t pb = (uint8_t)(((uint32_t)src_b * a + 127U) / 255U);
            row_buf[col - col_start] = ((uint32_t)a << 24) | ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
        }

        er_blit_copy(
            row_buf, (col_end - col_start) * (int)sizeof(uint32_t), origin_x + col_start, fy, col_end - col_start, 1);
    }
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_text_render(const char* text, ERRect clip, uint32_t color, uint8_t font_size, const char* font_family)
{
    if (!text || ((color >> 24) & 0xFFU) == 0U)
        return;

    if (font_size < 8U)
        font_size = 8U;
    if (font_size > 96U)
        font_size = 96U;

    const BitmapFont* font = font_registry_get(font_family, font_size);
    if (!font)
        return;

    const int line_h = font->line_height;
    int cursor_x = clip.x;
    int cursor_y = clip.y;

    const char* p = text;
    while (*p)
    {
        const uint32_t cp = utf8_next(&p);

        if (cp == (uint32_t)'\n')
        {
            cursor_x = clip.x;
            cursor_y += line_h;
            if (cursor_y >= clip.y + clip.h)
                break;
            continue;
        }

        const GlyphInfo* g = font_glyph(font, cp);

        if (cursor_x + (int)g->advance > clip.x + clip.w)
        {
            cursor_x = clip.x;
            cursor_y += line_h;
            if (cursor_y >= clip.y + clip.h)
                break;
        }

        if (font->format != ERUI_FONT_FMT_1BIT)
            draw_glyph_aa(g, font->bitmap, font->format, cursor_x, cursor_y, &clip, color);
        else
            draw_glyph(g, font->bitmap, cursor_x, cursor_y, &clip, color);
        cursor_x += g->advance;
    }
}

void er_text_measure(const char* text, uint8_t font_size, const char* font_family, int* out_width, int* out_height)
{
    if (font_size < 8U)
        font_size = 8U;
    if (font_size > 96U)
        font_size = 96U;

    const BitmapFont* font = font_registry_get(font_family, font_size);
    if (!font)
    {
        if (out_width)
            *out_width = 0;
        if (out_height)
            *out_height = (int)font_size;
        return;
    }

    long width = 0;
    if (text)
    {
        const char* p = text;
        while (*p)
        {
            const uint32_t cp = utf8_next(&p);
            if (cp == (uint32_t)'\n')
                continue;
            const GlyphInfo* g = font_glyph(font, cp);
            if (g)
                width += g->advance;
        }
    }
    if (width > INT_MAX)
        width = INT_MAX;

    if (out_width)
        *out_width = (int)width;
    if (out_height)
        *out_height = (int)font->line_height;
}
