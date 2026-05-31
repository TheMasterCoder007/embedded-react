#include "text_renderer.h"
#include "font_bitmap.h"
#include "font_registry.h"
#include "renderer_internal.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Maximum line count tracked per er_text_render() call. */
#define TEXT_MAX_LINES 64

/** @brief Per-row italic shear factor.  Each row shifts right by (g->height - 1 - row) * this value. */
#define ITALIC_SLOPE 0.2f

/** @brief UTF-8 encoding of U+2026 HORIZONTAL ELLIPSIS '…'. */
#define ELLIPSIS_UTF8 "\xE2\x80\xA6"

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief A single rendered line produced by break_lines().
 */
typedef struct
{
    const char* start; /**< Pointer to first byte of this line in the source string. */
    int byte_len;      /**< Number of bytes in this line (trailing whitespace excluded). */
    int px_width;      /**< Pixel width of this line (trailing whitespace excluded). */
} LineSpan;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Decodes the next UTF-8 codepoint and advances the pointer.
 *
 * Supports 1-byte (ASCII), 2-byte, and 3-byte sequences. 4-byte sequences
 * (above U+FFFF) are consumed in full and return U+003F ('?'). Ill-formed bytes
 * advance by one and return U+003F ('?').
 *
 * @param[in,out] pp  Pointer to the current position; advanced past the decoded sequence.
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
 * @brief Returns the pixel advance for a codepoint including letter_spacing.
 *
 * @param[in] font          BitmapFont to look up the glyph in.
 * @param[in] cp            Unicode codepoint.
 * @param[in] letter_spacing Extra pixels added to the glyph's natural advance.
 *
 * @return Total cursor advance in pixels.
 */
static int glyph_adv(const BitmapFont* font, uint32_t cp, int letter_spacing)
{
    return (int)font_glyph(font, cp)->advance + letter_spacing;
}

/**
 * @brief Draws a single glyph bitmap into the framebuffer via run-length fill calls.
 *
 * Pixels are emitted as horizontal runs of set bits using er_blit_fill(). All
 * output is clipped to the supplied clip rectangle.  When italic is true each row
 * is shifted right by (height - 1 - row) * ITALIC_SLOPE pixels producing a shear slant.
 *
 * @param[in] g         GlyphInfo describing the glyph dimensions and bitmap offset.
 * @param[in] bitmap    Pointer to the font's packed 1-bit-per-pixel bitmap data.
 * @param[in] cursor_x  Cursor X origin (left of the advance box) in framebuffer pixels.
 * @param[in] cursor_y  Cursor Y origin (top of the line box) in framebuffer pixels.
 * @param[in] clip      Clipping rectangle; no pixels are drawn outside this area.
 * @param[in] color     Text color as straight-alpha ARGB8888.
 * @param[in] italic    When true, apply a synthetic italic shear to each row.
 */
static void draw_glyph(const GlyphInfo* g,
                       const uint8_t* bitmap,
                       int cursor_x,
                       int cursor_y,
                       const ERRect* clip,
                       uint32_t color,
                       bool italic)
{
    if (g->width == 0U || g->height == 0U)
        return;

    const uint8_t* bmp = bitmap + g->bitmap_offset;
    const int row_bytes = (g->width + 7) >> 3;

    const int clip_x1 = clip->x;
    const int clip_y1 = clip->y;
    const int clip_x2 = clip->x + clip->w;
    const int clip_y2 = clip->y + clip->h;

    const int base_origin_x = cursor_x + g->x_offset;
    const int origin_y = cursor_y + g->y_offset;

    for (int row = 0; row < g->height; row++)
    {
        const int fy = origin_y + row;
        if (fy < clip_y1 || fy >= clip_y2)
            continue;

        const int italic_dx = italic ? (int)((float)(g->height - 1 - row) * ITALIC_SLOPE) : 0;
        const int origin_x = base_origin_x + italic_dx;

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
 * When italic is true, a bilinear subpixel shear is applied per row: source pixel at
 * column s contributes (1 − frac) to destination column (s + shift_int) and frac to
 * (s + shift_int + 1), where shift = (height − 1 − row) × ITALIC_SLOPE.  This
 * eliminates the staircase edge artefact produced by integer-only shifts.
 *
 * @param[in] g         GlyphInfo describing the glyph dimensions and bitmap offset.
 * @param[in] bitmap    Pointer to the font's packed grayscale bitmap data.
 * @param[in] bpp       Bits per pixel of the font bitmap (2, 4, or 8).
 * @param[in] cursor_x  Cursor X origin (left of the advance box) in framebuffer pixels.
 * @param[in] cursor_y  Cursor Y origin (top of the line box) in framebuffer pixels.
 * @param[in] clip      Clipping rectangle; no pixels are drawn outside this area.
 * @param[in] color     Text color as straight-alpha ARGB8888.
 * @param[in] italic    When true, apply a smooth subpixel italic shear to each row.
 */
static void draw_glyph_aa(const GlyphInfo* g,
                          const uint8_t* bitmap,
                          uint8_t bpp,
                          int cursor_x,
                          int cursor_y,
                          const ERRect* clip,
                          uint32_t color,
                          bool italic)
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
    const int base_origin_x = cursor_x + g->x_offset;
    const int origin_y = cursor_y + g->y_offset;

    const int row_stride = (bpp == 8) ? (int)g->width : (bpp == 4) ? ((int)g->width + 1) / 2 : ((int)g->width + 3) / 4;

    /* Scratch buffers: cov_buf holds unpacked source coverage; out_cov holds the
     * (optionally blended) coverage that feeds the final premultiplied row; row_buf
     * is the premultiplied ARGB output passed to er_blit_copy.  All sized for the
     * maximum glyph width (256) plus one extra slot for the italic fractional tail. */
    uint8_t cov_buf[256];
    uint8_t out_cov_buf[257];
    uint32_t row_buf[257];

    for (int row = 0; row < (int)g->height; row++)
    {
        const int fy = origin_y + row;
        if (fy < clip_y1 || fy >= clip_y2)
            continue;

        /* Per-row italic shift: integer part moves origin_x; fractional part feeds
         * the bilinear blend below. */
        const float italic_shift_f = italic ? (float)(g->height - 1 - row) * ITALIC_SLOPE : 0.0f;
        const int shift_int = (int)italic_shift_f;
        const float shift_frac = italic_shift_f - (float)shift_int;
        const int origin_x = base_origin_x + shift_int;

        /* Step 1 — unpack source coverage for the entire glyph row. */
        const uint8_t* src_row = bmp + (size_t)row * (size_t)row_stride;
        for (int col = 0; col < (int)g->width; col++)
        {
            uint8_t cov;
            if (bpp == 8)
            {
                cov = src_row[col];
            }
            else if (bpp == 4)
            {
                const uint8_t nibble = (col & 1) ? src_row[col >> 1] & 0x0FU : src_row[col >> 1] >> 4;
                cov = nibble * 17U;
            }
            else
            {
                const uint8_t pair = (src_row[col >> 2] >> (6U - ((uint8_t)col & 3U) * 2U)) & 0x03U;
                cov = pair * 85U;
            }
            cov_buf[col] = cov;
        }

        /* Step 2 — bilinear subpixel blend for italic (no-op when shift_frac ≈ 0).
         *
         * Destination column d receives:
         *   out_cov[d] = cov[d] * (1 - frac) + cov[d-1] * frac
         * where cov[x] = 0 for x outside [0, width-1].
         *
         * This distributes each source pixel between its two nearest destination
         * columns, producing anti-aliased shear edges instead of a staircase. */
        const uint8_t* out_cov;
        int out_width;
        if (!italic || shift_frac < 0.005f)
        {
            out_cov = cov_buf;
            out_width = (int)g->width;
        }
        else
        {
            out_width = (int)g->width + 1;
            for (int d = 0; d < out_width; d++)
            {
                const float cv_prev = (d > 0) ? (float)cov_buf[d - 1] : 0.0f;
                const float cv_curr = (d < (int)g->width) ? (float)cov_buf[d] : 0.0f;
                out_cov_buf[d] = (uint8_t)(cv_curr * (1.0f - shift_frac) + cv_prev * shift_frac + 0.5f);
            }
            out_cov = out_cov_buf;
        }

        /* Step 3 — clip and build premultiplied output row, then blit. */
        const int col_start = (clip_x1 > origin_x) ? clip_x1 - origin_x : 0;
        const int col_end = (clip_x2 < origin_x + out_width) ? clip_x2 - origin_x : out_width;
        if (col_start >= col_end)
            continue;

        for (int d = col_start; d < col_end; d++)
        {
            const uint8_t a = (uint8_t)(((uint32_t)src_a * out_cov[d] + 127U) / 255U);
            const uint8_t pr = (uint8_t)(((uint32_t)src_r * a + 127U) / 255U);
            const uint8_t pg = (uint8_t)(((uint32_t)src_g * a + 127U) / 255U);
            const uint8_t pb = (uint8_t)(((uint32_t)src_b * a + 127U) / 255U);
            row_buf[d - col_start] = ((uint32_t)a << 24) | ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
        }

        er_blit_copy(
            row_buf, (col_end - col_start) * (int)sizeof(uint32_t), origin_x + col_start, fy, col_end - col_start, 1);
    }
}

/**
 * @brief Draws a single codepoint glyph choosing between 1-bit and anti-aliased paths.
 *
 * @param[in] font      BitmapFont to look up the glyph in.
 * @param[in] cp        Unicode codepoint to render.
 * @param[in] cursor_x  Cursor X origin in framebuffer pixels.
 * @param[in] cursor_y  Cursor Y origin (top of line box) in framebuffer pixels.
 * @param[in] clip      Clipping rectangle.
 * @param[in] color     Straight-alpha ARGB8888 text color.
 * @param[in] italic    When true, apply a synthetic italic shear.
 */
static void draw_cp(
    const BitmapFont* font, uint32_t cp, int cursor_x, int cursor_y, const ERRect* clip, uint32_t color, bool italic)
{
    const GlyphInfo* g = font_glyph(font, cp);
    if (font->format != ERUI_FONT_FMT_1BIT)
        draw_glyph_aa(g, font->bitmap, font->format, cursor_x, cursor_y, clip, color, italic);
    else
        draw_glyph(g, font->bitmap, cursor_x, cursor_y, clip, color, italic);
}

/**
 * @brief Breaks a UTF-8 string into line spans that fit within max_w pixels.
 *
 * Wraps on word boundaries (spaces/tabs). Falls back to character-boundary wrapping
 * when a single word exceeds max_w. Explicit newlines always end a line. Leading
 * whitespace at the start of each wrapped line is consumed silently.
 *
 * @param[in]  text           Null-terminated UTF-8 source string.
 * @param[in]  font           BitmapFont used to measure glyph advances.
 * @param[in]  max_w          Maximum pixel width per line; 0 = no horizontal limit.
 * @param[in]  letter_spacing Extra pixels added to each glyph advance.
 * @param[in]  max_lines      Maximum lines to produce; 0 = unlimited.
 * @param[out] out            Caller-provided array to receive the line spans.
 * @param[in]  cap            Capacity of out[].
 * @param[out] out_truncated  Set to true when text was cut short by max_lines.
 *
 * @return Number of spans written to out[].
 */
static int break_lines(const char* text,
                       const BitmapFont* font,
                       int max_w,
                       int letter_spacing,
                       int max_lines,
                       LineSpan* out,
                       int cap,
                       bool* out_truncated)
{
    int n = 0;
    const char* p = text;
    *out_truncated = false;

    while (*p && n < cap)
    {
        /* Check line cap before starting a new line. */
        if (max_lines > 0 && n >= max_lines)
        {
            *out_truncated = (*p != '\0');
            break;
        }

        /* Skip leading horizontal whitespace for this wrapped line. */
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        /* Bare newline → empty line. */
        if (*p == '\n')
        {
            out[n++] = (LineSpan){p, 0, 0};
            p++;
            continue;
        }

        const char* line_start = p;
        const char* word_end = p;  /* end of last complete word (exclusive) */
        const char* next_word = p; /* start of next word after whitespace */
        int word_end_w = 0;
        int line_w = 0;
        bool saw_ws = false;       /* line has seen any whitespace (governs wrap break) */
        bool ends_with_ws = false; /* most recent chars consumed were whitespace */
        const char* ws_start = p;  /* position of the last run of trailing whitespace */
        int ws_start_w = 0;        /* line width up to ws_start */

        for (;;)
        {
            if (!*p)
            {
                /* End of string: commit the full line, but trim any trailing whitespace
                 * so labels like "Hello   " render as "Hello" without phantom advance. */
                if (ends_with_ws)
                    out[n++] = (LineSpan){line_start, (int)(ws_start - line_start), ws_start_w};
                else
                    out[n++] = (LineSpan){line_start, (int)(p - line_start), line_w};
                goto outer_break;
            }

            if (*p == '\n')
            {
                /* Explicit newline: same trim rule as end-of-string. */
                if (ends_with_ws)
                    out[n++] = (LineSpan){line_start, (int)(ws_start - line_start), ws_start_w};
                else
                    out[n++] = (LineSpan){line_start, (int)(p - line_start), line_w};
                p++;
                break; /* outer while picks up the next line */
            }

            if (*p == ' ' || *p == '\t')
            {
                /* Record end of the current word before consuming whitespace. */
                word_end = p;
                word_end_w = line_w;
                if (!ends_with_ws)
                {
                    ws_start = p;
                    ws_start_w = line_w;
                }
                while (*p == ' ' || *p == '\t')
                {
                    uint32_t cp = utf8_next(&p);
                    line_w += glyph_adv(font, cp, letter_spacing);
                }
                next_word = p;
                saw_ws = true;
                ends_with_ws = true;
                continue;
            }

            /* Non-whitespace: a word character. */
            const char* cp_start = p;
            uint32_t cp = utf8_next(&p);
            const int adv = glyph_adv(font, cp, letter_spacing);

            if (max_w > 0 && line_w + adv > max_w && cp_start > line_start)
            {
                if (saw_ws)
                {
                    /* Break at the last word boundary. */
                    out[n++] = (LineSpan){line_start, (int)(word_end - line_start), word_end_w};
                    p = next_word;
                }
                else
                {
                    /* No word boundary found: character-boundary break. */
                    out[n++] = (LineSpan){line_start, (int)(cp_start - line_start), line_w};
                    p = cp_start;
                }
                break;
            }

            line_w += adv;
            ends_with_ws = false;
        }
    }

    return n;

outer_break:
    return n;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_text_render(const ERTextRenderParams* params)
{
    if (!params || !params->text || ((params->color >> 24) & 0xFFU) == 0U)
        return;
    if (params->clip.w <= 0 || params->clip.h <= 0)
        return;

    uint8_t sz = params->font_size;
    if (sz < 8U)
        sz = 8U;
    if (sz > 96U)
        sz = 96U;

    const BitmapFont* font = font_registry_get(params->font_family, sz);
    if (!font)
        return;

    const int lh = (params->line_height > 0) ? (int)params->line_height : (int)font->line_height;
    const int ls = (int)params->letter_spacing;
    const bool bold = (params->font_weight != 0U);
    const bool italic = (params->font_style != 0U);

    /* Build line spans. */
    LineSpan spans[TEXT_MAX_LINES];
    bool truncated = false;
    const int n_lines = break_lines(
        params->text, font, params->clip.w, ls, (int)params->number_of_lines, spans, TEXT_MAX_LINES, &truncated);
    if (n_lines == 0)
        return;

    /* Pre-compute ellipsis glyph for TAIL mode. */
    const bool do_ellipsis = truncated && (params->ellipsize_mode != ER_TEXT_ELLIPSIZE_CLIP);
    int ellipsis_px = 0;
    uint32_t ellipsis_cp = 0;
    const GlyphInfo* ellipsis_g = NULL;

    if (do_ellipsis)
    {
        const char* ep = ELLIPSIS_UTF8;
        ellipsis_cp = utf8_next(&ep);
        ellipsis_g = font_glyph(font, ellipsis_cp);
        ellipsis_px = (int)ellipsis_g->advance + ls + (bold ? 1 : 0);
    }

    /* Render each line. */
    for (int i = 0; i < n_lines; i++)
    {
        const int cursor_y = params->clip.y + i * lh;
        if (cursor_y >= params->clip.y + params->clip.h)
            break;

        const bool is_last = (i == n_lines - 1);
        const bool apply_ellip = (do_ellipsis && is_last);

        /* Determine the visible text range.  For the ellipsis line we shorten
         * the span so that text + '…' fits within clip.w. */
        const char* render_end = spans[i].start + spans[i].byte_len;
        int render_w = spans[i].px_width;

        if (apply_ellip)
        {
            const int avail = params->clip.w - ellipsis_px;
            const char* p = spans[i].start;
            const char* cut = p;
            int w = 0;
            while (p < render_end && *p)
            {
                const char* cps = p;
                uint32_t cp = utf8_next(&p);
                const int adv = glyph_adv(font, cp, ls) + (bold ? 1 : 0);
                if (w + adv > avail)
                {
                    p = cps;
                    break;
                }
                w += adv;
                cut = p;
            }
            render_end = cut;
            render_w = w;
        }

        /* Compute horizontal cursor from alignment. */
        int cursor_x;
        if (apply_ellip || params->text_align == ER_TEXT_ALIGN_LEFT)
        {
            cursor_x = params->clip.x;
        }
        else if (params->text_align == ER_TEXT_ALIGN_CENTER)
        {
            const int off = (params->clip.w - spans[i].px_width) / 2;
            cursor_x = params->clip.x + (off > 0 ? off : 0);
        }
        else /* ER_TEXT_ALIGN_RIGHT */
        {
            const int off = params->clip.w - spans[i].px_width;
            cursor_x = params->clip.x + (off > 0 ? off : 0);
        }

        /* Draw text glyphs. */
        {
            const char* p = spans[i].start;
            int cx = cursor_x;
            while (p < render_end && *p)
            {
                uint32_t cp = utf8_next(&p);
                draw_cp(font, cp, cx, cursor_y, &params->clip, params->color, italic);
                if (bold)
                    draw_cp(font, cp, cx + 1, cursor_y, &params->clip, params->color, italic);
                cx += glyph_adv(font, cp, ls) + (bold ? 1 : 0);
            }
        }

        /* Draw ellipsis glyph. */
        if (apply_ellip && ellipsis_g)
        {
            const int ecx = cursor_x + render_w;
            draw_cp(font, ellipsis_cp, ecx, cursor_y, &params->clip, params->color, italic);
            if (bold)
                draw_cp(font, ellipsis_cp, ecx + 1, cursor_y, &params->clip, params->color, italic);
        }

        /* Text decoration (underline / line-through). */
        if (params->text_decoration != ER_TEXT_DECORATION_NONE)
        {
            int dec_w = apply_ellip ? (render_w + ellipsis_px) : spans[i].px_width;
            const int right_edge = params->clip.x + params->clip.w;
            if (cursor_x + dec_w > right_edge)
                dec_w = right_edge - cursor_x;
            if (dec_w > 0)
            {
                int dec_y;
                if (params->text_decoration == ER_TEXT_DECORATION_UNDERLINE)
                    dec_y = cursor_y + (int)font->baseline + 1;
                else /* LINE_THROUGH */
                    dec_y = cursor_y + (int)font->baseline * 2 / 3;

                if (dec_y >= params->clip.y && dec_y < params->clip.y + params->clip.h)
                    er_blit_fill(params->color, cursor_x, dec_y, dec_w, 1);
            }
        }
    }
}

void er_text_measure(const char* text,
                     uint8_t font_size,
                     const char* font_family,
                     int16_t letter_spacing,
                     int* out_width,
                     int* out_height)
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
            width += glyph_adv(font, cp, (int)letter_spacing);
        }
    }
    if (width > INT_MAX)
        width = INT_MAX;

    if (out_width)
        *out_width = (int)width;
    if (out_height)
        *out_height = (int)font->line_height;
}
