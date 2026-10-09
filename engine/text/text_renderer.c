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

/** @brief Pixels per er_blit_copy() call in draw_glyph_aa(); sizes its row buffer on the stack. */
#define GLYPH_ROW_CHUNK 64

/** @brief Capacity of the buffer that span mode merges every span's text into. */
#define SPAN_MERGED_MAX (ER_TEXT_MAX_SPANS * (ER_SPAN_TEXT_MAX + 1))

/** @brief UTF-8 encoding of U+2026 HORIZONTAL ELLIPSIS '…'. */
#define ELLIPSIS_UTF8 "\xE2\x80\xA6"

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief A single rendered line produced by next_line().
 */
typedef struct
{
    const char* start; /**< Pointer to first byte of this line in the source string. */
    int byte_len;      /**< Number of bytes in this line (trailing whitespace excluded). */
    int px_width;      /**< Pixel width of this line (trailing whitespace excluded). */
} LineSpan;

/**
 * @brief Line-breaking state for next_line(): the text still to break and the limits that apply.
 */
typedef struct
{
    const char* p;          /**< First byte not yet assigned to a line. */
    const BitmapFont* font; /**< Font used to measure glyph advances. */
    int max_w;              /**< Maximum pixel width per line; 0 = no horizontal limit. */
    int letter_spacing;     /**< Extra pixels added to each glyph advance. */
    int max_lines;          /**< Maximum lines to produce; 0 = unlimited. */
    int count;              /**< Lines produced so far. */
    bool truncated;         /**< Set when max_lines cut the text short. */
} LineBreaker;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Diagnostic: number of glyph-run measurement calls (er_text_measure + er_text_measure_spans)
 * since process start. Exposed via er_text_measure_count() so callers (and tests) can confirm
 * the layout pass's measure_content() cache is actually skipping redundant remeasurement. */
static uint32_t s_text_measure_count = 0;

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
            cp = (cp << 6) | (b & 0x3FU);
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
 * @brief Unpacks a run of grayscale glyph coverage and scales it to 0-255.
 *
 * 2-bit values scale by 85, 4-bit by 17, and 8-bit values are copied as they are.
 *
 * @param[in]  src_row  First byte of the glyph row in the packed bitmap.
 * @param[in]  bpp      Bits per pixel of the font bitmap (2, 4, or 8).
 * @param[in]  col      First column to unpack.
 * @param[in]  n        Number of columns; all of [col, col + n) lie inside the glyph.
 * @param[out] out      Receives n coverage values.
 */
static inline void unpack_cov(const uint8_t* src_row, uint8_t bpp, int col, int n, uint8_t* out)
{
    if (bpp == 8)
    {
        memcpy(out, src_row + col, (size_t)n);
    }
    else if (bpp == 4)
    {
        for (int k = 0; k < n; k++)
        {
            const int c = col + k;
            const uint8_t nibble = (c & 1) ? src_row[c >> 1] & 0x0FU : src_row[c >> 1] >> 4;
            out[k] = (uint8_t)(nibble * 17U);
        }
    }
    else
    {
        for (int k = 0; k < n; k++)
        {
            const int c = col + k;
            const uint8_t pair = (src_row[c >> 2] >> (6U - ((uint8_t)c & 3U) * 2U)) & 0x03U;
            out[k] = (uint8_t)(pair * 85U);
        }
    }
}

/**
 * @brief (v + 127) / 255 for v in [0, 255 * 255], without a divide: the Cortex-M0+ has no divide instruction.
 *
 * @param[in] v  Product of two 8-bit values.
 *
 * @return The quotient, in [0, 255].
 */
static inline uint32_t div255(uint32_t v)
{
    v += 128U;
    return (v + (v >> 8)) >> 8;
}

/**
 * @brief Premultiplied ARGB8888 for a straight-alpha color drawn at one coverage value.
 *
 * @param[in] a    Color alpha.
 * @param[in] r    Color red.
 * @param[in] g    Color green.
 * @param[in] b    Color blue.
 * @param[in] cov  Coverage in [0, 255].
 *
 * @return The premultiplied pixel.
 */
static inline uint32_t premul_cov(uint32_t a, uint32_t r, uint32_t g, uint32_t b, uint32_t cov)
{
    const uint32_t pa = div255(a * cov);
    return (pa << 24) | (div255(r * pa) << 16) | (div255(g * pa) << 8) | div255(b * pa);
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

    const int width = (int)g->width;
    const int row_stride = (bpp == 8) ? width : (bpp == 4) ? (width + 1) / 2 : (width + 3) / 4;

    /* Per-chunk scratch: italic coverage (cov[0] is the column before the chunk) and premultiplied ARGB. */
    uint8_t cov[GLYPH_ROW_CHUNK + 1];
    uint32_t row_buf[GLYPH_ROW_CHUNK];

    /* 2- and 4-bit coverage has only 4 or 16 levels, so each level is premultiplied once per glyph. */
    uint32_t level_px[16];
    if (bpp != 8)
    {
        const int levels = (bpp == 4) ? 16 : 4;
        const uint32_t scale = (bpp == 4) ? 17U : 85U;
        for (int v = 0; v < levels; v++)
            level_px[v] = premul_cov(src_a, src_r, src_g, src_b, (uint32_t)v * scale);
    }

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

        /* Bilinear subpixel blend for italic (skipped when shift_frac ≈ 0).
         *
         * Destination column d receives:
         *   out_cov[d] = cov[d] * (1 - frac) + cov[d-1] * frac
         * where cov[x] = 0 for x outside [0, width-1].
         *
         * This distributes each source pixel between its two nearest destination
         * columns, producing anti-aliased shear edges instead of a staircase. */
        const bool blend = italic && shift_frac >= 0.005f;
        const int out_width = blend ? width + 1 : width;

        const int col_start = (clip_x1 > origin_x) ? clip_x1 - origin_x : 0;
        const int col_end = (clip_x2 < origin_x + out_width) ? clip_x2 - origin_x : out_width;
        if (col_start >= col_end)
            continue;

        /* Coverage is unpacked only for the visible columns. cov[0] is the column before the chunk. */
        const uint8_t* src_row = bmp + (size_t)row * (size_t)row_stride;
        cov[0] = 0U;
        if (blend && col_start > 0)
            unpack_cov(src_row, bpp, col_start - 1, 1, cov);

        for (int chunk = col_start; chunk < col_end; chunk += GLYPH_ROW_CHUNK)
        {
            const int n = (col_end - chunk < GLYPH_ROW_CHUNK) ? col_end - chunk : GLYPH_ROW_CHUNK;

            if (blend)
            {
                /* Only the blend's tail column lies past the glyph, and it has no coverage of its own. */
                const int n_src = (chunk + n > width) ? width - chunk : n;
                unpack_cov(src_row, bpp, chunk, n_src, cov + 1);
                for (int k = n_src; k < n; k++)
                    cov[k + 1] = 0U;
                for (int k = 0; k < n; k++)
                {
                    const float cv_prev = (float)cov[k];
                    const float cv_curr = (float)cov[k + 1];
                    row_buf[k] = premul_cov(src_a,
                                            src_r,
                                            src_g,
                                            src_b,
                                            (uint8_t)(cv_curr * (1.0f - shift_frac) + cv_prev * shift_frac + 0.5f));
                }
                cov[0] = cov[n];
            }
            else if (bpp == 8)
            {
                const uint8_t* src = src_row + chunk;
                for (int k = 0; k < n; k++)
                    row_buf[k] = premul_cov(src_a, src_r, src_g, src_b, src[k]);
            }
            else if (bpp == 4)
            {
                for (int k = 0; k < n; k++)
                {
                    const int c = chunk + k;
                    row_buf[k] = level_px[(c & 1) ? src_row[c >> 1] & 0x0FU : src_row[c >> 1] >> 4];
                }
            }
            else
            {
                for (int k = 0; k < n; k++)
                {
                    const int c = chunk + k;
                    row_buf[k] = level_px[(src_row[c >> 2] >> (6U - ((uint8_t)c & 3U) * 2U)) & 0x03U];
                }
            }
            er_blit_copy(row_buf, n * (int)sizeof(uint32_t), origin_x + chunk, fy, n, 1);
        }
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
 * @brief Produces the next line of a UTF-8 string that fits within the breaker's max_w pixels.
 *
 * Wraps on word boundaries (spaces/tabs). Falls back to character-boundary wrapping
 * when a single word exceeds max_w. Explicit newlines always end a line. Leading
 * whitespace at the start of each wrapped line is consumed silently. At most
 * TEXT_MAX_LINES lines are produced.
 *
 * @param[in,out] lb   Breaker state; advanced past the returned line.
 * @param[out]    out  Receives the line span.
 *
 * @return true when a line was produced; false once the text or the line budget is used up.
 */
static bool next_line(LineBreaker* lb, LineSpan* out)
{
    const BitmapFont* font = lb->font;
    const int max_w = lb->max_w;
    const int letter_spacing = lb->letter_spacing;
    const char* p = lb->p;

    if (!*p || lb->count >= TEXT_MAX_LINES)
        return false;

    /* Check line cap before starting a new line. */
    if (lb->max_lines > 0 && lb->count >= lb->max_lines)
    {
        lb->truncated = true;
        return false;
    }

    /* Skip leading horizontal whitespace for this wrapped line. */
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
    {
        lb->p = p;
        return false;
    }

    /* Bare newline → empty line. */
    if (*p == '\n')
    {
        *out = (LineSpan){p, 0, 0};
        lb->p = p + 1;
        lb->count++;
        return true;
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
                *out = (LineSpan){line_start, (int)(ws_start - line_start), ws_start_w};
            else
                *out = (LineSpan){line_start, (int)(p - line_start), line_w};
            break;
        }

        if (*p == '\n')
        {
            /* Explicit newline: same trim rule as end-of-string. */
            if (ends_with_ws)
                *out = (LineSpan){line_start, (int)(ws_start - line_start), ws_start_w};
            else
                *out = (LineSpan){line_start, (int)(p - line_start), line_w};
            p++;
            break;
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
                *out = (LineSpan){line_start, (int)(word_end - line_start), word_end_w};
                p = next_word;
            }
            else
            {
                /* No word boundary found: character-boundary break. */
                *out = (LineSpan){line_start, (int)(cp_start - line_start), line_w};
                p = cp_start;
            }
            break;
        }

        line_w += adv;
        ends_with_ws = false;
    }

    lb->p = p;
    lb->count++;
    return true;
}

/**
 * @brief Breaks a resolved text run into lines and draws them.
 *
 * Lines are broken one ahead of the one being drawn, so only the last line's lookahead decides the
 * ellipsis, and nothing below the clip is broken at all. Not inlined: both callers share one copy.
 *
 * @param[in] params    Render parameters (clip, color, alignment, decoration, spans).
 * @param[in] font      Font resolved from params.
 * @param[in] src       UTF-8 text to draw: params->text, or every span's text merged into one string.
 * @param[in] span_end  Span mode: the byte offset in src where each span ends. NULL draws src as one run.
 * @param[in] n_spans   Number of entries in span_end.
 */
static ER_NOINLINE void render_lines(const ERTextRenderParams* params,
                                     const BitmapFont* font,
                                     const char* src,
                                     const uint16_t* span_end,
                                     uint8_t n_spans)
{
    const int lh = (params->line_height > 0) ? (int)params->line_height : (int)font->line_height;
    const int ls = (int)params->letter_spacing;
    const bool bold = (params->font_weight != 0U);
    const bool italic = (params->font_style != 0U);

    /* Line breaking always uses the parent ls for measurement. */
    LineBreaker lb = {src, font, params->clip.w, ls, (int)params->number_of_lines, 0, false};
    LineSpan line;
    LineSpan next;
    bool have_next = next_line(&lb, &next);

    /* ---- Render each line. ---- */
    for (int i = 0; have_next; i++)
    {
        line = next;
        const int cursor_y = params->clip.y + i * lh;
        if (cursor_y >= params->clip.y + params->clip.h)
            break;

        have_next = next_line(&lb, &next);
        const bool is_last = !have_next;
        const bool apply_ellip = is_last && lb.truncated && (params->ellipsize_mode != ER_TEXT_ELLIPSIZE_CLIP);

        /* Ellipsis glyph for TAIL mode. */
        int ellipsis_px = 0;
        uint32_t ellipsis_cp = 0;
        const GlyphInfo* ellipsis_g = NULL;
        if (apply_ellip)
        {
            const char* ep = ELLIPSIS_UTF8;
            ellipsis_cp = utf8_next(&ep);
            ellipsis_g = font_glyph(font, ellipsis_cp);
            ellipsis_px = (int)ellipsis_g->advance + ls + (bold ? 1 : 0);
        }

        /* Determine the visible text range; shorten for ellipsis. */
        const char* render_end = line.start + line.byte_len;
        int render_w = line.px_width;

        if (apply_ellip)
        {
            const int avail = params->clip.w - ellipsis_px;
            const char* p = line.start;
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
            const int off = (params->clip.w - line.px_width) / 2;
            cursor_x = params->clip.x + (off > 0 ? off : 0);
        }
        else /* ER_TEXT_ALIGN_RIGHT */
        {
            const int off = params->clip.w - line.px_width;
            cursor_x = params->clip.x + (off > 0 ? off : 0);
        }

        if (span_end)
        {
            /* ---- Span-aware rendering: group consecutive chars of the same span. ---- */
            const char* p = line.start;
            int cx = cursor_x;

            while (p < render_end && *p)
            {
                const size_t byte_off = (size_t)(p - src);
                uint8_t si = 0;
                while (si + 1U < n_spans && byte_off >= span_end[si])
                    si++;
                const ERTextSpan* sp = &params->spans[si];

                /* Resolve per-span style; sentinels inherit from base params. */
                const uint32_t seg_color = sp->color ? sp->color : params->color;
                const bool seg_bold = (sp->font_weight == 0xFFU) ? bold : (sp->font_weight != 0U);
                const bool seg_italic = (sp->font_style == 0xFFU) ? italic : (sp->font_style != 0U);
                const int seg_ls = (sp->letter_spacing == ER_LAYOUT_AUTO) ? ls : (int)sp->letter_spacing;
                const uint8_t seg_deco = (sp->text_decoration == 0xFFU) ? params->text_decoration : sp->text_decoration;

                const int seg_start_cx = cx;

                /* Render all characters of this span run within the line. */
                while (p < render_end && *p)
                {
                    if ((size_t)(p - src) >= span_end[si])
                        break;
                    uint32_t cp = utf8_next(&p);
                    draw_cp(font, cp, cx, cursor_y, &params->clip, seg_color, seg_italic);
                    if (seg_bold)
                        draw_cp(font, cp, cx + 1, cursor_y, &params->clip, seg_color, seg_italic);
                    cx += glyph_adv(font, cp, seg_ls) + (seg_bold ? 1 : 0);
                }
                /* Text decoration for this span segment. */
                if (seg_deco != ER_TEXT_DECORATION_NONE)
                {
                    int dec_w = cx - seg_start_cx;
                    const int right_edge = params->clip.x + params->clip.w;
                    if (seg_start_cx + dec_w > right_edge)
                        dec_w = right_edge - seg_start_cx;
                    if (dec_w > 0)
                    {
                        const int dec_y = (seg_deco == ER_TEXT_DECORATION_UNDERLINE)
                                              ? cursor_y + (int)font->baseline + 1
                                              : cursor_y + (int)font->baseline * 2 / 3;
                        if (dec_y >= params->clip.y && dec_y < params->clip.y + params->clip.h)
                            er_blit_fill(seg_color, seg_start_cx, dec_y, dec_w, 1);
                    }
                }
            }

            /* Ellipsis glyph uses parent style. */
            if (apply_ellip && ellipsis_g)
            {
                draw_cp(font, ellipsis_cp, cx, cursor_y, &params->clip, params->color, italic);
                if (bold)
                    draw_cp(font, ellipsis_cp, cx + 1, cursor_y, &params->clip, params->color, italic);
            }
        }
        else
        {
            /* ---- Single-run path. ---- */
            {
                const char* p = line.start;
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

            if (apply_ellip && ellipsis_g)
            {
                const int ecx = cursor_x + render_w;
                draw_cp(font, ellipsis_cp, ecx, cursor_y, &params->clip, params->color, italic);
                if (bold)
                    draw_cp(font, ellipsis_cp, ecx + 1, cursor_y, &params->clip, params->color, italic);
            }

            if (params->text_decoration != ER_TEXT_DECORATION_NONE)
            {
                int dec_w = apply_ellip ? (render_w + ellipsis_px) : line.px_width;
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
}

/**
 * @brief Span mode: merges every span's text into one string and draws it as one run of lines.
 *
 * Kept out of er_text_render() so plain text does not carry the merge buffer on its stack.
 *
 * @param[in] params  Render parameters; spans[] provides the content (at most ER_TEXT_MAX_SPANS are drawn).
 * @param[in] font    Font resolved from params.
 */
static ER_NOINLINE void render_span_text(const ERTextRenderParams* params, const BitmapFont* font)
{
    char merged[SPAN_MERGED_MAX + 1];
    uint16_t span_end[ER_TEXT_MAX_SPANS];
    const uint8_t n_spans = (params->span_count < ER_TEXT_MAX_SPANS) ? params->span_count : ER_TEXT_MAX_SPANS;

    size_t pos = 0;
    for (uint8_t si = 0; si < n_spans; si++)
    {
        const char* s = params->spans[si].text;
        while (*s && pos < SPAN_MERGED_MAX)
            merged[pos++] = *s++;
        span_end[si] = (uint16_t)pos;
    }
    merged[pos] = '\0';

    if (merged[0])
        render_lines(params, font, merged, span_end, n_spans);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_text_render(const ERTextRenderParams* params)
{
    if (!params || ((params->color >> 24) & 0xFFU) == 0U)
        return;
    if (params->clip.w <= 0 || params->clip.h <= 0)
        return;

    /* Span mode: params->text is ignored; spans[] provides the content. */
    const bool span_mode = (params->span_count > 0 && params->spans != NULL);
    if (!span_mode && !params->text)
        return;

    const uint8_t sz = er_text_clamp_font_size(params->font_size);

    const BitmapFont* font = font_registry_get(params->font_family, sz);
    if (!font)
        return;

    if (span_mode)
        render_span_text(params, font);
    else if (params->text[0])
        render_lines(params, font, params->text, NULL, 0);
}

void er_text_measure(const char* text,
                     uint8_t font_size,
                     const char* font_family,
                     int16_t letter_spacing,
                     uint8_t font_weight,
                     int* out_width,
                     int* out_height)
{
    s_text_measure_count++;

    font_size = er_text_clamp_font_size(font_size);

    const BitmapFont* font = font_registry_get(font_family, font_size);
    if (!font)
    {
        if (out_width)
            *out_width = 0;
        if (out_height)
            *out_height = (int)font_size;
        return;
    }

    /* Bold is synthesised by drawing each glyph twice 1px apart, which the renderer accounts for
       in the cursor advance (+1px/glyph).  Measure the same way so the box fits the drawn text. */
    const int bold_extra = (font_weight != 0U) ? 1 : 0;
    long width = 0;
    if (text)
    {
        const char* p = text;
        while (*p)
        {
            const uint32_t cp = utf8_next(&p);
            if (cp == (uint32_t)'\n')
                continue;
            width += glyph_adv(font, cp, (int)letter_spacing) + bold_extra;
        }
    }
    if (width > INT_MAX)
        width = INT_MAX;

    if (out_width)
        *out_width = (int)width;
    if (out_height)
        *out_height = (int)font->line_height;
}

void er_text_measure_spans(const ERTextSpan* spans,
                           uint8_t span_count,
                           uint8_t font_size,
                           const char* font_family,
                           int16_t base_letter_spacing,
                           uint8_t base_font_weight,
                           int* out_width,
                           int* out_height)
{
    s_text_measure_count++;

    font_size = er_text_clamp_font_size(font_size);

    const BitmapFont* font = font_registry_get(font_family, font_size);
    if (!font || !spans || span_count == 0U)
    {
        if (out_width)
            *out_width = 0;
        if (out_height)
            *out_height = font ? (int)font->line_height : (int)font_size;
        return;
    }

    const bool base_bold = (base_font_weight != 0U);
    long width = 0;
    for (uint8_t si = 0; si < span_count; si++)
    {
        const ERTextSpan* sp = &spans[si];
        /* Resolve per-span style from sentinels exactly as the renderer does. */
        const bool seg_bold = (sp->font_weight == 0xFFU) ? base_bold : (sp->font_weight != 0U);
        const int seg_ls = (sp->letter_spacing == ER_LAYOUT_AUTO) ? (int)base_letter_spacing : (int)sp->letter_spacing;
        const int bold_extra = seg_bold ? 1 : 0;

        const char* p = sp->text;
        while (*p)
        {
            const uint32_t cp = utf8_next(&p);
            if (cp == (uint32_t)'\n')
                continue;
            width += glyph_adv(font, cp, seg_ls) + bold_extra;
        }
    }
    if (width > INT_MAX)
        width = INT_MAX;

    if (out_width)
        *out_width = (int)width;
    if (out_height)
        *out_height = (int)font->line_height;
}

uint32_t er_text_measure_count(void)
{
    return s_text_measure_count;
}
