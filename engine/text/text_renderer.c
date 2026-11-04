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

/** @brief Returned for a sequence that is ill-formed or above U+FFFF. */
#define UTF8_REPLACEMENT '?'

/* UTF-8 lead bytes: a byte is the lead of an N-byte sequence when (byte & MASK) == BITS. */
#define UTF8_ONE_BYTE_LIMIT 0x80U
#define UTF8_LEAD2_MASK 0xE0U
#define UTF8_LEAD2_BITS 0xC0U
#define UTF8_LEAD2_PAYLOAD 0x1FU
#define UTF8_LEAD3_MASK 0xF0U
#define UTF8_LEAD3_BITS 0xE0U
#define UTF8_LEAD3_PAYLOAD 0x0FU
#define UTF8_LEAD4_MASK 0xF8U
#define UTF8_LEAD4_BITS 0xF0U

/* UTF-8 continuation bytes each carry 6 payload bits. */
#define UTF8_CONTINUATION_MASK 0xC0U
#define UTF8_CONTINUATION_BITS 0x80U
#define UTF8_CONTINUATION_PAYLOAD 0x3FU
#define UTF8_CONTINUATION_SHIFT 6

/** @brief A span style byte holding this inherits the base style. */
#define TEXT_SPAN_INHERIT 0xFFU

/** @brief Scales a 2- or 4-bit coverage value to 0-255. */
#define COVERAGE_SCALE_2BIT 85U
#define COVERAGE_SCALE_4BIT 17U

/** @brief Number of levels 2- and 4-bit coverage has. */
#define COVERAGE_LEVELS_2BIT 4
#define COVERAGE_LEVELS_4BIT 16

/** @brief An italic shift smaller than this fraction of a pixel skips the subpixel blend. */
#define ITALIC_BLEND_MIN_FRACTION 0.005f

/** @brief A line-through sits this fraction of the baseline below the top of the line. */
#define LINE_THROUGH_NUMERATOR 2
#define LINE_THROUGH_DENOMINATOR 3

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief A single rendered line produced by next_line().
 */
typedef struct
{
    const char* start; /**< Pointer to first byte of this line in the source string. */
    int byte_length;   /**< Number of bytes in this line (trailing whitespace excluded). */
    int pixel_width;   /**< Pixel width of this line (trailing whitespace excluded). */
} LineSpan;

/**
 * @brief Line-breaking state for next_line(): the text still to break and the limits that apply.
 */
typedef struct
{
    const char* cursor;       /**< First byte not yet assigned to a line. */
    const BitmapFont* font;   /**< Font used to measure glyph advances. */
    int max_width;            /**< Maximum pixel width per line; 0 = no horizontal limit. */
    int letter_spacing;       /**< Base extra pixels added to each glyph advance. */
    bool bold;                /**< Base faux bold: each glyph is drawn one pixel wider. */
    int max_lines;            /**< Maximum lines to produce; 0 = unlimited. */
    int count;                /**< Lines produced so far. */
    bool truncated;           /**< Set when max_lines cut the text short. */
    const char* text_start;   /**< Start of the text; span_end offsets count from here. */
    const ERTextSpan* spans;  /**< Span styles overriding the base; NULL for a single run. */
    const uint16_t* span_end; /**< Byte offset in text_start where each span ends; NULL for a single run. */
    uint8_t span_count;       /**< Number of entries in spans and span_end. */
} LineBreaker;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/* Diagnostic: number of glyph-run measurement calls (er_text_measure, er_text_measure_spans and
 * er_text_measure_lines) since process start. Exposed via er_text_measure_count() so callers (and
 * tests) can confirm the layout pass's measure_content() cache is actually skipping redundant
 * remeasurement. */
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
 * @param[in,out] cursor  Pointer to the current position; advanced past the decoded sequence.
 *
 * @return The decoded Unicode codepoint, or U+003F on error.
 */
static uint32_t utf8_next(const char** cursor)
{
    const uint8_t* bytes = (const uint8_t*)*cursor;
    const uint8_t lead_byte = bytes[0];
    uint32_t codepoint = 0;
    int sequence_length = 0;
    int beyond_bmp = 0;

    if (lead_byte < UTF8_ONE_BYTE_LIMIT)
    {
        sequence_length = 1;
        codepoint = lead_byte;
    }
    else if ((lead_byte & UTF8_LEAD2_MASK) == UTF8_LEAD2_BITS)
    {
        sequence_length = 2;
        codepoint = lead_byte & UTF8_LEAD2_PAYLOAD;
    }
    else if ((lead_byte & UTF8_LEAD3_MASK) == UTF8_LEAD3_BITS)
    {
        sequence_length = 3;
        codepoint = lead_byte & UTF8_LEAD3_PAYLOAD;
    }
    else if ((lead_byte & UTF8_LEAD4_MASK) == UTF8_LEAD4_BITS)
    {
        sequence_length = 4;
        beyond_bmp = 1;
    }
    else
    {
        *cursor = (const char*)(bytes + 1);
        return UTF8_REPLACEMENT;
    }

    for (int byte_index = 1; byte_index < sequence_length; byte_index++)
    {
        const uint8_t continuation_byte = bytes[byte_index];
        if (continuation_byte == 0)
        {
            *cursor = (const char*)(bytes + byte_index);
            return UTF8_REPLACEMENT;
        }
        if ((continuation_byte & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION_BITS)
        {
            *cursor = (const char*)(bytes + byte_index);
            return UTF8_REPLACEMENT;
        }
        if (!beyond_bmp)
            codepoint = (codepoint << UTF8_CONTINUATION_SHIFT) | (continuation_byte & UTF8_CONTINUATION_PAYLOAD);
    }
    *cursor = (const char*)(bytes + sequence_length);
    return beyond_bmp ? (uint32_t)UTF8_REPLACEMENT : codepoint;
}

/**
 * @brief Returns the pixel advance for a codepoint including letter_spacing.
 *
 * @param[in] font            BitmapFont to look up the glyph in.
 * @param[in] codepoint       Unicode codepoint.
 * @param[in] letter_spacing  Extra pixels added to the glyph's natural advance.
 *
 * @return Total cursor advance in pixels.
 */
static int glyph_adv(const BitmapFont* font, uint32_t codepoint, int letter_spacing)
{
    return (int)font_glyph(font, codepoint)->advance + letter_spacing;
}

/**
 * @brief Draws a single glyph bitmap into the framebuffer via run-length fill calls.
 *
 * Pixels are emitted as horizontal runs of set bits using er_blit_fill(). All
 * output is clipped to the supplied clip rectangle.  When italic is true each row
 * is shifted right by (height - 1 - row) * ITALIC_SLOPE pixels producing a shear slant.
 *
 * @param[in] glyph     GlyphInfo describing the glyph dimensions and bitmap offset.
 * @param[in] bitmap    Pointer to the font's packed 1-bit-per-pixel bitmap data.
 * @param[in] cursor_x  Cursor X origin (left of the advance box) in framebuffer pixels.
 * @param[in] cursor_y  Cursor Y origin (top of the line box) in framebuffer pixels.
 * @param[in] clip      Clipping rectangle; no pixels are drawn outside this area.
 * @param[in] color     Text color as straight-alpha ARGB8888.
 * @param[in] italic    When true, apply a synthetic italic shear to each row.
 */
static void draw_glyph(const GlyphInfo* glyph,
                       const uint8_t* bitmap,
                       int cursor_x,
                       int cursor_y,
                       const ERRect* clip,
                       uint32_t color,
                       bool italic)
{
    if (glyph->width == 0U || glyph->height == 0U)
        return;

    const uint8_t* glyph_bitmap = bitmap + glyph->bitmap_offset;
    const int row_bytes = (glyph->width + 7) >> 3;

    const int clip_left = clip->x;
    const int clip_top = clip->y;
    const int clip_right = clip->x + clip->width;
    const int clip_bottom = clip->y + clip->height;

    const int base_origin_x = cursor_x + glyph->x_offset;
    const int origin_y = cursor_y + glyph->y_offset;

    for (int row = 0; row < glyph->height; row++)
    {
        const int pixel_y = origin_y + row;
        if (pixel_y < clip_top || pixel_y >= clip_bottom)
            continue;

        const int italic_shift = italic ? (int)((float)(glyph->height - 1 - row) * ITALIC_SLOPE) : 0;
        const int origin_x = base_origin_x + italic_shift;

        const uint8_t* source_row = glyph_bitmap + (size_t)row * (size_t)row_bytes;
        int run_start = -1;

        for (int column = 0; column < glyph->width; column++)
        {
            const int pixel_x = origin_x + column;
            const int in_clip_x = (pixel_x >= clip_left && pixel_x < clip_right);
            const uint8_t pixel_set = source_row[column >> 3] & (uint8_t)(0x80U >> (column & 7));

            if (pixel_set && in_clip_x)
            {
                if (run_start < 0)
                    run_start = pixel_x;
            }
            else if (run_start >= 0)
            {
                er_blit_fill(color, run_start, pixel_y, pixel_x - run_start, 1);
                run_start = -1;
            }
        }

        if (run_start >= 0)
        {
            const int run_end = origin_x + glyph->width;
            const int clipped_end = (run_end < clip_right) ? run_end : clip_right;
            er_blit_fill(color, run_start, pixel_y, clipped_end - run_start, 1);
        }
    }
}

/**
 * @brief Unpacks a run of grayscale glyph coverage and scales it to 0-255.
 *
 * 2-bit values scale by 85, 4-bit by 17, and 8-bit values are copied as they are.
 *
 * @param[in]  source_row      First byte of the glyph row in the packed bitmap.
 * @param[in]  bits_per_pixel  Bits per pixel of the font bitmap (2, 4, or 8).
 * @param[in]  first_column    First column to unpack.
 * @param[in]  column_count    Number of columns; all of [first_column, first_column + column_count) lie inside the
 * glyph.
 * @param[out] coverage        Receives column_count coverage values.
 */
static inline void
unpack_cov(const uint8_t* source_row, uint8_t bits_per_pixel, int first_column, int column_count, uint8_t* coverage)
{
    if (bits_per_pixel == 8)
    {
        memcpy(coverage, source_row + first_column, (size_t)column_count);
    }
    else if (bits_per_pixel == 4)
    {
        for (int offset = 0; offset < column_count; offset++)
        {
            const int column = first_column + offset;
            const uint8_t nibble = (column & 1) ? source_row[column >> 1] & 0x0FU : source_row[column >> 1] >> 4;
            coverage[offset] = (uint8_t)(nibble * COVERAGE_SCALE_4BIT);
        }
    }
    else
    {
        for (int offset = 0; offset < column_count; offset++)
        {
            const int column = first_column + offset;
            const uint8_t pair = (source_row[column >> 2] >> (6U - ((uint8_t)column & 3U) * 2U)) & 0x03U;
            coverage[offset] = (uint8_t)(pair * COVERAGE_SCALE_2BIT);
        }
    }
}

/**
 * @brief (product + 127) / 255 for product in [0, 255 * 255], without a divide: the Cortex-M0+ has no divide
 * instruction.
 *
 * @param[in] product  Product of two 8-bit values.
 *
 * @return The quotient, in [0, 255].
 */
static inline uint32_t div255(uint32_t product)
{
    product += 128U;
    return (product + (product >> 8)) >> 8;
}

/**
 * @brief Premultiplied ARGB8888 for a straight-alpha color drawn at one coverage value.
 *
 * @param[in] alpha     Color alpha.
 * @param[in] red       Color red.
 * @param[in] green     Color green.
 * @param[in] blue      Color blue.
 * @param[in] coverage  Coverage in [0, 255].
 *
 * @return The premultiplied pixel.
 */
static inline uint32_t premul_cov(uint32_t alpha, uint32_t red, uint32_t green, uint32_t blue, uint32_t coverage)
{
    const uint32_t premultiplied_alpha = div255(alpha * coverage);
    return (premultiplied_alpha << 24) | (div255(red * premultiplied_alpha) << 16)
           | (div255(green * premultiplied_alpha) << 8) | div255(blue * premultiplied_alpha);
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
 * @param[in] glyph           GlyphInfo describing the glyph dimensions and bitmap offset.
 * @param[in] bitmap          Pointer to the font's packed grayscale bitmap data.
 * @param[in] bits_per_pixel  Bits per pixel of the font bitmap (2, 4, or 8).
 * @param[in] cursor_x        Cursor X origin (left of the advance box) in framebuffer pixels.
 * @param[in] cursor_y        Cursor Y origin (top of the line box) in framebuffer pixels.
 * @param[in] clip            Clipping rectangle; no pixels are drawn outside this area.
 * @param[in] color           Text color as straight-alpha ARGB8888.
 * @param[in] italic          When true, apply a smooth subpixel italic shear to each row.
 */
static void draw_glyph_aa(const GlyphInfo* glyph,
                          const uint8_t* bitmap,
                          uint8_t bits_per_pixel,
                          int cursor_x,
                          int cursor_y,
                          const ERRect* clip,
                          uint32_t color,
                          bool italic)
{
    if (glyph->width == 0U || glyph->height == 0U)
        return;

    const uint8_t color_alpha = (uint8_t)(color >> 24);
    const uint8_t color_red = (uint8_t)(color >> 16);
    const uint8_t color_green = (uint8_t)(color >> 8);
    const uint8_t color_blue = (uint8_t)(color);

    const uint8_t* glyph_bitmap = bitmap + glyph->bitmap_offset;
    const int clip_left = clip->x;
    const int clip_top = clip->y;
    const int clip_right = clip->x + clip->width;
    const int clip_bottom = clip->y + clip->height;
    const int base_origin_x = cursor_x + glyph->x_offset;
    const int origin_y = cursor_y + glyph->y_offset;

    const int width = (int)glyph->width;
    const int row_stride = (bits_per_pixel == 8) ? width : (bits_per_pixel == 4) ? (width + 1) / 2 : (width + 3) / 4;

    /* Per-chunk scratch: italic coverage (coverage[0] is the column before the chunk) and premultiplied ARGB. */
    uint8_t coverage[GLYPH_ROW_CHUNK + 1];
    uint32_t row_pixels[GLYPH_ROW_CHUNK];

    /* 2- and 4-bit coverage has only 4 or 16 levels, so each level is premultiplied once per glyph. */
    uint32_t level_pixels[COVERAGE_LEVELS_4BIT];
    if (bits_per_pixel != 8)
    {
        const int levels = (bits_per_pixel == 4) ? COVERAGE_LEVELS_4BIT : COVERAGE_LEVELS_2BIT;
        const uint32_t scale = (bits_per_pixel == 4) ? COVERAGE_SCALE_4BIT : COVERAGE_SCALE_2BIT;
        for (int level = 0; level < levels; level++)
            level_pixels[level] = premul_cov(color_alpha, color_red, color_green, color_blue, (uint32_t)level * scale);
    }

    for (int row = 0; row < (int)glyph->height; row++)
    {
        const int pixel_y = origin_y + row;
        if (pixel_y < clip_top || pixel_y >= clip_bottom)
            continue;

        /* Per-row italic shift: integer part moves origin_x; fractional part feeds
         * the bilinear blend below. */
        const float italic_shift = italic ? (float)(glyph->height - 1 - row) * ITALIC_SLOPE : 0.0f;
        const int shift_int = (int)italic_shift;
        const float shift_fraction = italic_shift - (float)shift_int;
        const int origin_x = base_origin_x + shift_int;

        /* Bilinear subpixel blend for italic (skipped when shift_fraction ≈ 0).
         *
         * Destination column d receives:
         *   out_cov[d] = coverage[d] * (1 - frac) + coverage[d-1] * frac
         * where coverage[x] = 0 for x outside [0, width-1].
         *
         * This distributes each source pixel between its two nearest destination
         * columns, producing anti-aliased shear edges instead of a staircase. */
        const bool blend = italic && shift_fraction >= ITALIC_BLEND_MIN_FRACTION;
        const int out_width = blend ? width + 1 : width;

        const int column_start = (clip_left > origin_x) ? clip_left - origin_x : 0;
        const int column_end = (clip_right < origin_x + out_width) ? clip_right - origin_x : out_width;
        if (column_start >= column_end)
            continue;

        /* Coverage is unpacked only for the visible columns. coverage[0] is the column before the chunk. */
        const uint8_t* source_row = glyph_bitmap + (size_t)row * (size_t)row_stride;
        coverage[0] = 0U;
        if (blend && column_start > 0)
            unpack_cov(source_row, bits_per_pixel, column_start - 1, 1, coverage);

        for (int chunk = column_start; chunk < column_end; chunk += GLYPH_ROW_CHUNK)
        {
            const int chunk_columns = (column_end - chunk < GLYPH_ROW_CHUNK) ? column_end - chunk : GLYPH_ROW_CHUNK;

            if (blend)
            {
                /* Only the blend's tail column lies past the glyph, and it has no coverage of its own. */
                const int source_columns = (chunk + chunk_columns > width) ? width - chunk : chunk_columns;
                unpack_cov(source_row, bits_per_pixel, chunk, source_columns, coverage + 1);
                for (int offset = source_columns; offset < chunk_columns; offset++)
                    coverage[offset + 1] = 0U;
                for (int offset = 0; offset < chunk_columns; offset++)
                {
                    const float previous_coverage = (float)coverage[offset];
                    const float current_coverage = (float)coverage[offset + 1];
                    row_pixels[offset] = premul_cov(color_alpha,
                                                    color_red,
                                                    color_green,
                                                    color_blue,
                                                    (uint8_t)(current_coverage * (1.0f - shift_fraction)
                                                              + previous_coverage * shift_fraction + 0.5f));
                }
                coverage[0] = coverage[chunk_columns];
            }
            else if (bits_per_pixel == 8)
            {
                const uint8_t* source_coverage = source_row + chunk;
                for (int offset = 0; offset < chunk_columns; offset++)
                    row_pixels[offset] =
                        premul_cov(color_alpha, color_red, color_green, color_blue, source_coverage[offset]);
            }
            else if (bits_per_pixel == 4)
            {
                for (int offset = 0; offset < chunk_columns; offset++)
                {
                    const int column = chunk + offset;
                    row_pixels[offset] =
                        level_pixels[(column & 1) ? source_row[column >> 1] & 0x0FU : source_row[column >> 1] >> 4];
                }
            }
            else
            {
                for (int offset = 0; offset < chunk_columns; offset++)
                {
                    const int column = chunk + offset;
                    row_pixels[offset] =
                        level_pixels[(source_row[column >> 2] >> (6U - ((uint8_t)column & 3U) * 2U)) & 0x03U];
                }
            }
            er_blit_copy(
                row_pixels, chunk_columns * (int)sizeof(uint32_t), origin_x + chunk, pixel_y, chunk_columns, 1);
        }
    }
}

/**
 * @brief Draws a single codepoint glyph choosing between 1-bit and anti-aliased paths.
 *
 * @param[in] font       BitmapFont to look up the glyph in.
 * @param[in] codepoint  Unicode codepoint to render.
 * @param[in] cursor_x   Cursor X origin in framebuffer pixels.
 * @param[in] cursor_y   Cursor Y origin (top of line box) in framebuffer pixels.
 * @param[in] clip       Clipping rectangle.
 * @param[in] color      Straight-alpha ARGB8888 text color.
 * @param[in] italic     When true, apply a synthetic italic shear.
 */
static void draw_cp(const BitmapFont* font,
                    uint32_t codepoint,
                    int cursor_x,
                    int cursor_y,
                    const ERRect* clip,
                    uint32_t color,
                    bool italic)
{
    const GlyphInfo* glyph = font_glyph(font, codepoint);
    if (font->format != ERUI_FONT_FORMAT_1BIT)
        draw_glyph_aa(glyph, font->bitmap, font->format, cursor_x, cursor_y, clip, color, italic);
    else
        draw_glyph(glyph, font->bitmap, cursor_x, cursor_y, clip, color, italic);
}

/**
 * @brief Returns the advance a codepoint is drawn with: letter spacing and faux bold of the span holding it.
 *
 * Matches the draw loop in render_lines() and er_text_measure_spans(), so a line never draws wider than the
 * width it was broken at.
 *
 * @param[in] breaker    Breaker state supplying the font, the base style and the spans.
 * @param[in] position   Byte in breaker->text_start where the codepoint starts.
 * @param[in] codepoint  The codepoint.
 *
 * @return Cursor advance in pixels.
 */
static int drawn_adv(const LineBreaker* breaker, const char* position, uint32_t codepoint)
{
    int letter_spacing = breaker->letter_spacing;
    bool bold = breaker->bold;
    if (breaker->span_end)
    {
        const size_t byte_offset = (size_t)(position - breaker->text_start);
        uint8_t span_index = 0;
        while (span_index + 1U < breaker->span_count && byte_offset >= breaker->span_end[span_index])
            span_index++;
        const ERTextSpan* span = &breaker->spans[span_index];
        if (span->font_weight != TEXT_SPAN_INHERIT)
            bold = (span->font_weight != 0U);
        if (span->letter_spacing != ER_LAYOUT_AUTO)
            letter_spacing = (int)span->letter_spacing;
    }
    return glyph_adv(breaker->font, codepoint, letter_spacing) + (bold ? 1 : 0);
}

/**
 * @brief Produces the next line of a UTF-8 string that fits within the breaker's max_width pixels.
 *
 * Wraps on word boundaries (spaces/tabs). Falls back to character-boundary wrapping
 * when a single word exceeds max_width. Explicit newlines always end a line. Leading
 * whitespace at the start of each wrapped line is consumed silently. At most
 * TEXT_MAX_LINES lines are produced.
 *
 * @param[in,out] breaker  Breaker state; advanced past the returned line.
 * @param[out]    out      Receives the line span.
 *
 * @return true when a line was produced; false once the text or the line budget is used up.
 */
static bool next_line(LineBreaker* breaker, LineSpan* out)
{
    const int max_width = breaker->max_width;
    const char* cursor = breaker->cursor;

    if (!*cursor || breaker->count >= TEXT_MAX_LINES)
        return false;

    /* Check line cap before starting a new line. */
    if (breaker->max_lines > 0 && breaker->count >= breaker->max_lines)
    {
        breaker->truncated = true;
        return false;
    }

    /* Skip leading horizontal whitespace for this wrapped line. */
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    if (!*cursor)
    {
        breaker->cursor = cursor;
        return false;
    }

    /* Bare newline → empty line. */
    if (*cursor == '\n')
    {
        *out = (LineSpan){cursor, 0, 0};
        breaker->cursor = cursor + 1;
        breaker->count++;
        return true;
    }

    const char* line_start = cursor;
    const char* word_end = cursor;  /* end of last complete word (exclusive) */
    const char* next_word = cursor; /* start of next word after whitespace */
    int word_end_width = 0;
    int line_width = 0;
    bool saw_whitespace = false;           /* line has seen any whitespace (governs wrap break) */
    bool ends_with_whitespace = false;     /* most recent chars consumed were whitespace */
    const char* whitespace_start = cursor; /* position of the last run of trailing whitespace */
    int whitespace_start_width = 0;        /* line width up to whitespace_start */

    for (;;)
    {
        if (!*cursor)
        {
            /* End of string: commit the full line, but trim any trailing whitespace
             * so labels like "Hello   " render as "Hello" without phantom advance. */
            if (ends_with_whitespace)
                *out = (LineSpan){line_start, (int)(whitespace_start - line_start), whitespace_start_width};
            else
                *out = (LineSpan){line_start, (int)(cursor - line_start), line_width};
            break;
        }

        if (*cursor == '\n')
        {
            /* Explicit newline: same trim rule as end-of-string. */
            if (ends_with_whitespace)
                *out = (LineSpan){line_start, (int)(whitespace_start - line_start), whitespace_start_width};
            else
                *out = (LineSpan){line_start, (int)(cursor - line_start), line_width};
            cursor++;
            break;
        }

        if (*cursor == ' ' || *cursor == '\t')
        {
            /* Record end of the current word before consuming whitespace. */
            word_end = cursor;
            word_end_width = line_width;
            if (!ends_with_whitespace)
            {
                whitespace_start = cursor;
                whitespace_start_width = line_width;
            }
            while (*cursor == ' ' || *cursor == '\t')
            {
                const char* whitespace_position = cursor;
                uint32_t codepoint = utf8_next(&cursor);
                line_width += drawn_adv(breaker, whitespace_position, codepoint);
            }
            next_word = cursor;
            saw_whitespace = true;
            ends_with_whitespace = true;
            continue;
        }

        /* Non-whitespace: a word character. */
        const char* codepoint_start = cursor;
        uint32_t codepoint = utf8_next(&cursor);
        const int advance = drawn_adv(breaker, codepoint_start, codepoint);

        if (max_width > 0 && line_width + advance > max_width && codepoint_start > line_start)
        {
            if (saw_whitespace)
            {
                /* Break at the last word boundary. */
                *out = (LineSpan){line_start, (int)(word_end - line_start), word_end_width};
                cursor = next_word;
            }
            else
            {
                /* No word boundary found: character-boundary break. */
                *out = (LineSpan){line_start, (int)(codepoint_start - line_start), line_width};
                cursor = codepoint_start;
            }
            break;
        }

        line_width += advance;
        ends_with_whitespace = false;
    }

    breaker->cursor = cursor;
    breaker->count++;
    return true;
}

/**
 * @brief Breaks a resolved text run into lines and draws them.
 *
 * Lines are broken one ahead of the one being drawn, so only the last line's lookahead decides the
 * ellipsis, and nothing below the clip is broken at all. Not inlined: both callers share one copy.
 *
 * @param[in] params      Render parameters (clip, color, alignment, decoration, spans).
 * @param[in] font        Font resolved from params.
 * @param[in] text        UTF-8 text to draw: params->text, or every span's text merged into one string.
 * @param[in] span_end    Span mode: the byte offset in text where each span ends. NULL draws text as one run.
 * @param[in] span_count  Number of entries in span_end.
 */
static ER_NOINLINE void render_lines(const ERTextRenderParams* params,
                                     const BitmapFont* font,
                                     const char* text,
                                     const uint16_t* span_end,
                                     uint8_t span_count)
{
    const int line_height = (params->line_height > 0) ? (int)params->line_height : (int)font->line_height;
    const int letter_spacing = (int)params->letter_spacing;
    const bool bold = (params->font_weight != 0U);
    const bool italic = (params->font_style != 0U);

    /* Lines break at the advance each glyph is drawn with, span styles and faux bold included. */
    LineBreaker breaker = {text,
                           font,
                           params->clip.width,
                           letter_spacing,
                           bold,
                           (int)params->number_of_lines,
                           0,
                           false,
                           text,
                           params->spans,
                           span_end,
                           span_count};
    LineSpan line;
    LineSpan next;
    bool have_next = next_line(&breaker, &next);

    /* ---- Render each line. ---- */
    for (int line_index = 0; have_next; line_index++)
    {
        line = next;
        const int cursor_y = params->clip.y + line_index * line_height;
        if (cursor_y >= params->clip.y + params->clip.height)
            break;

        have_next = next_line(&breaker, &next);
        const bool is_last = !have_next;
        const bool apply_ellipsis = is_last && breaker.truncated && (params->ellipsize_mode != ER_TEXT_ELLIPSIZE_CLIP);

        /* Ellipsis glyph for TAIL mode. */
        int ellipsis_width = 0;
        uint32_t ellipsis_codepoint = 0;
        const GlyphInfo* ellipsis_glyph = NULL;
        if (apply_ellipsis)
        {
            const char* ellipsis_text = ELLIPSIS_UTF8;
            ellipsis_codepoint = utf8_next(&ellipsis_text);
            ellipsis_glyph = font_glyph(font, ellipsis_codepoint);
            ellipsis_width = (int)ellipsis_glyph->advance + letter_spacing + (bold ? 1 : 0);
        }

        /* Determine the visible text range; shorten for ellipsis. */
        const char* render_end = line.start + line.byte_length;
        int render_width = line.pixel_width;

        if (apply_ellipsis)
        {
            const int available_width = params->clip.width - ellipsis_width;
            const char* cursor = line.start;
            const char* cut_position = cursor;
            int kept_width = 0;
            while (cursor < render_end && *cursor)
            {
                const char* codepoint_start = cursor;
                uint32_t codepoint = utf8_next(&cursor);
                const int advance = drawn_adv(&breaker, codepoint_start, codepoint);
                if (kept_width + advance > available_width)
                {
                    cursor = codepoint_start;
                    break;
                }
                kept_width += advance;
                cut_position = cursor;
            }
            render_end = cut_position;
            render_width = kept_width;
        }

        /* Compute horizontal cursor from alignment. */
        int cursor_x;
        if (apply_ellipsis || params->text_align == ER_TEXT_ALIGN_LEFT)
        {
            cursor_x = params->clip.x;
        }
        else if (params->text_align == ER_TEXT_ALIGN_CENTER)
        {
            const int free_space = (params->clip.width - line.pixel_width) / 2;
            cursor_x = params->clip.x + (free_space > 0 ? free_space : 0);
        }
        else /* ER_TEXT_ALIGN_RIGHT */
        {
            const int free_space = params->clip.width - line.pixel_width;
            cursor_x = params->clip.x + (free_space > 0 ? free_space : 0);
        }

        if (span_end)
        {
            /* ---- Span-aware rendering: group consecutive chars of the same span. ---- */
            const char* cursor = line.start;
            int pen_x = cursor_x;

            while (cursor < render_end && *cursor)
            {
                const size_t byte_offset = (size_t)(cursor - text);
                uint8_t span_index = 0;
                while (span_index + 1U < span_count && byte_offset >= span_end[span_index])
                    span_index++;
                const ERTextSpan* span = &params->spans[span_index];

                /* Resolve per-span style; sentinels inherit from base params. */
                const uint32_t segment_color = span->color ? span->color : params->color;
                const bool segment_bold = (span->font_weight == TEXT_SPAN_INHERIT) ? bold : (span->font_weight != 0U);
                const bool segment_italic = (span->font_style == TEXT_SPAN_INHERIT) ? italic : (span->font_style != 0U);
                const int segment_letter_spacing =
                    (span->letter_spacing == ER_LAYOUT_AUTO) ? letter_spacing : (int)span->letter_spacing;
                const uint8_t segment_decoration =
                    (span->text_decoration == TEXT_SPAN_INHERIT) ? params->text_decoration : span->text_decoration;

                const int segment_start_x = pen_x;

                /* Render all characters of this span run within the line. */
                while (cursor < render_end && *cursor)
                {
                    if ((size_t)(cursor - text) >= span_end[span_index])
                        break;
                    uint32_t codepoint = utf8_next(&cursor);
                    draw_cp(font, codepoint, pen_x, cursor_y, &params->clip, segment_color, segment_italic);
                    if (segment_bold)
                        draw_cp(font, codepoint, pen_x + 1, cursor_y, &params->clip, segment_color, segment_italic);
                    pen_x += glyph_adv(font, codepoint, segment_letter_spacing) + (segment_bold ? 1 : 0);
                }
                /* Text decoration for this span segment. */
                if (segment_decoration != ER_TEXT_DECORATION_NONE)
                {
                    int decoration_width = pen_x - segment_start_x;
                    const int right_edge = params->clip.x + params->clip.width;
                    if (segment_start_x + decoration_width > right_edge)
                        decoration_width = right_edge - segment_start_x;
                    if (decoration_width > 0)
                    {
                        const int decoration_y =
                            (segment_decoration == ER_TEXT_DECORATION_UNDERLINE)
                                ? cursor_y + (int)font->baseline + 1
                                : cursor_y + (int)font->baseline * LINE_THROUGH_NUMERATOR / LINE_THROUGH_DENOMINATOR;
                        if (decoration_y >= params->clip.y && decoration_y < params->clip.y + params->clip.height)
                            er_blit_fill(segment_color, segment_start_x, decoration_y, decoration_width, 1);
                    }
                }
            }

            /* Ellipsis glyph uses parent style. */
            if (apply_ellipsis && ellipsis_glyph)
            {
                draw_cp(font, ellipsis_codepoint, pen_x, cursor_y, &params->clip, params->color, italic);
                if (bold)
                    draw_cp(font, ellipsis_codepoint, pen_x + 1, cursor_y, &params->clip, params->color, italic);
            }
        }
        else
        {
            /* ---- Single-run path. ---- */
            {
                const char* cursor = line.start;
                int pen_x = cursor_x;
                while (cursor < render_end && *cursor)
                {
                    uint32_t codepoint = utf8_next(&cursor);
                    draw_cp(font, codepoint, pen_x, cursor_y, &params->clip, params->color, italic);
                    if (bold)
                        draw_cp(font, codepoint, pen_x + 1, cursor_y, &params->clip, params->color, italic);
                    pen_x += glyph_adv(font, codepoint, letter_spacing) + (bold ? 1 : 0);
                }
            }

            if (apply_ellipsis && ellipsis_glyph)
            {
                const int ellipsis_x = cursor_x + render_width;
                draw_cp(font, ellipsis_codepoint, ellipsis_x, cursor_y, &params->clip, params->color, italic);
                if (bold)
                    draw_cp(font, ellipsis_codepoint, ellipsis_x + 1, cursor_y, &params->clip, params->color, italic);
            }

            if (params->text_decoration != ER_TEXT_DECORATION_NONE)
            {
                int decoration_width = apply_ellipsis ? (render_width + ellipsis_width) : line.pixel_width;
                const int right_edge = params->clip.x + params->clip.width;
                if (cursor_x + decoration_width > right_edge)
                    decoration_width = right_edge - cursor_x;
                if (decoration_width > 0)
                {
                    int decoration_y;
                    if (params->text_decoration == ER_TEXT_DECORATION_UNDERLINE)
                        decoration_y = cursor_y + (int)font->baseline + 1;
                    else /* LINE_THROUGH */
                        decoration_y =
                            cursor_y + (int)font->baseline * LINE_THROUGH_NUMERATOR / LINE_THROUGH_DENOMINATOR;

                    if (decoration_y >= params->clip.y && decoration_y < params->clip.y + params->clip.height)
                        er_blit_fill(params->color, cursor_x, decoration_y, decoration_width, 1);
                }
            }
        }
    }
}

/**
 * @brief Merges every span's text into one string, recording where each span ends.
 *
 * @param[in]  params    Render parameters; spans[] provides the content (at most ER_TEXT_MAX_SPANS are used).
 * @param[out] merged    Receives the merged, null-terminated text; SPAN_MERGED_MAX + 1 bytes.
 * @param[out] span_end  Receives the byte offset in merged where each span ends; ER_TEXT_MAX_SPANS entries.
 *
 * @return The number of spans merged.
 */
static uint8_t merge_spans(const ERTextRenderParams* params, char* merged, uint16_t* span_end)
{
    const uint8_t span_count = (params->span_count < ER_TEXT_MAX_SPANS) ? params->span_count : ER_TEXT_MAX_SPANS;

    size_t length = 0;
    for (uint8_t span_index = 0; span_index < span_count; span_index++)
    {
        const char* span_text = params->spans[span_index].text;
        while (*span_text && length < SPAN_MERGED_MAX)
            merged[length++] = *span_text++;
        span_end[span_index] = (uint16_t)length;
    }
    merged[length] = '\0';
    return span_count;
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
    const uint8_t span_count = merge_spans(params, merged, span_end);

    if (merged[0])
        render_lines(params, font, merged, span_end, span_count);
}

/**
 * @brief Breaks a resolved text run into lines the way render_lines() does, without drawing them.
 *
 * @param[in]  params      Text style: letter spacing, faux bold, number_of_lines and spans.
 * @param[in]  font        Font resolved from params.
 * @param[in]  text        UTF-8 text to break: params->text, or every span's text merged into one string.
 * @param[in]  span_end    Span mode: the byte offset in text where each span ends. NULL breaks text as one run.
 * @param[in]  span_count  Number of entries in span_end.
 * @param[in]  max_width   Width to break at in pixels; 0 = break at explicit newlines only.
 * @param[out] out_width   Receives the widest line's width in pixels.
 * @param[out] out_lines   Receives the number of lines.
 */
static void break_lines(const ERTextRenderParams* params,
                        const BitmapFont* font,
                        const char* text,
                        const uint16_t* span_end,
                        uint8_t span_count,
                        int max_width,
                        int* out_width,
                        int* out_lines)
{
    LineBreaker breaker = {text,
                           font,
                           max_width,
                           (int)params->letter_spacing,
                           params->font_weight != 0U,
                           (int)params->number_of_lines,
                           0,
                           false,
                           text,
                           params->spans,
                           span_end,
                           span_count};
    LineSpan line;
    int widest = 0;
    int lines = 0;
    while (next_line(&breaker, &line))
    {
        if (line.pixel_width > widest)
            widest = line.pixel_width;
        lines++;
    }
    *out_width = widest;
    *out_lines = lines;
}

/**
 * @brief Span mode of er_text_measure_lines(): merges the spans and breaks them as one run.
 *
 * Kept out of er_text_measure_lines() so plain text does not carry the merge buffer on its stack.
 *
 * @param[in]  params     Text and style; spans[] provides the content.
 * @param[in]  font       Font resolved from params.
 * @param[in]  max_width  Width to break at in pixels; 0 = break at explicit newlines only.
 * @param[out] out_width  Receives the widest line's width in pixels.
 * @param[out] out_lines  Receives the number of lines.
 */
static ER_NOINLINE void break_span_text(const ERTextRenderParams* params,
                                        const BitmapFont* font,
                                        int max_width,
                                        int* out_width,
                                        int* out_lines)
{
    char merged[SPAN_MERGED_MAX + 1];
    uint16_t span_end[ER_TEXT_MAX_SPANS];
    const uint8_t span_count = merge_spans(params, merged, span_end);

    break_lines(params, font, merged, span_end, span_count, max_width, out_width, out_lines);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void er_text_render(const ERTextRenderParams* params)
{
    if (!params || ((params->color >> 24) & 0xFFU) == 0U)
        return;
    if (params->clip.width <= 0 || params->clip.height <= 0)
        return;

    /* Span mode: params->text is ignored; spans[] provides the content. */
    const bool span_mode = (params->span_count > 0 && params->spans != NULL);
    if (!span_mode && !params->text)
        return;

    const uint8_t clamped_size = er_text_clamp_font_size(params->font_size);

    const BitmapFont* font = font_registry_get(params->font_family, clamped_size);
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
        const char* cursor = text;
        while (*cursor)
        {
            const uint32_t codepoint = utf8_next(&cursor);
            if (codepoint == (uint32_t)'\n')
                continue;
            width += glyph_adv(font, codepoint, (int)letter_spacing) + bold_extra;
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
    for (uint8_t span_index = 0; span_index < span_count; span_index++)
    {
        const ERTextSpan* span = &spans[span_index];
        /* Resolve per-span style from sentinels exactly as the renderer does. */
        const bool segment_bold = (span->font_weight == TEXT_SPAN_INHERIT) ? base_bold : (span->font_weight != 0U);
        const int segment_letter_spacing =
            (span->letter_spacing == ER_LAYOUT_AUTO) ? (int)base_letter_spacing : (int)span->letter_spacing;
        const int bold_extra = segment_bold ? 1 : 0;

        const char* cursor = span->text;
        while (*cursor)
        {
            const uint32_t codepoint = utf8_next(&cursor);
            if (codepoint == (uint32_t)'\n')
                continue;
            width += glyph_adv(font, codepoint, segment_letter_spacing) + bold_extra;
        }
    }
    if (width > INT_MAX)
        width = INT_MAX;

    if (out_width)
        *out_width = (int)width;
    if (out_height)
        *out_height = (int)font->line_height;
}

void er_text_measure_lines(const ERTextRenderParams* params, int max_width, int* out_width, int* out_lines)
{
    s_text_measure_count++;

    *out_width = 0;
    *out_lines = 0;

    const bool span_mode = (params->span_count > 0 && params->spans != NULL);
    if (!span_mode && !params->text)
        return;

    const BitmapFont* font = font_registry_get(params->font_family, er_text_clamp_font_size(params->font_size));
    if (!font)
        return;

    if (max_width < 0)
        max_width = 0;
    if (span_mode)
        break_span_text(params, font, max_width, out_width, out_lines);
    else
        break_lines(params, font, params->text, NULL, 0, max_width, out_width, out_lines);
}

uint32_t er_text_measure_count(void)
{
    return s_text_measure_count;
}
