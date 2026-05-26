#ifndef EMBEDDED_REACT_FONT_BITMAP_H
#define EMBEDDED_REACT_FONT_BITMAP_H

#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Constants
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief BitmapFont format: packed 1-bit-per-pixel glyph bitmaps (MSB-first). */
#define ERUI_FONT_FMT_1BIT 1

/** @brief BitmapFont format: packed 2-bit-per-pixel grayscale (4 pixels/byte, MSB pair = left). */
#define ERUI_FONT_FMT_2BIT 2

/** @brief BitmapFont format: packed 4-bit-per-pixel grayscale (2 pixels/byte, high nibble = left). */
#define ERUI_FONT_FMT_4BIT 4

/** @brief BitmapFont format: 8-bit grayscale glyph bitmaps (1 byte per pixel). */
#define ERUI_FONT_FMT_8BIT 8

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Metrics and bitmap location for a single glyph.
 *
 * bitmap_offset is a uint32_t so multi-BPP font atlases can address offsets beyond
 * the uint16_t range. The struct is 12 bytes on all targets (4-byte aligned).
 */
typedef struct
{
    uint32_t bitmap_offset; /**< Byte offset into the font bitmap array. */
    uint8_t width; /**< Glyph bitmap width in pixels. */
    uint8_t height; /**< Glyph bitmap height in pixels. */
    int8_t x_offset; /**< Horizontal bearing from the cursor origin to the glyph's left edge. */
    int8_t y_offset; /**< Vertical bearing from the cursor baseline to the glyph's top edge. */
    uint8_t advance; /**< Cursor advance in pixels after drawing this glyph. */
    uint8_t _pad[3]; /**< Explicit padding to maintain 12-byte struct size. */
} GlyphInfo;

/**
 * @brief A codepoint-to-glyph mapping entry for sparse (non-ASCII) glyphs.
 *
 * ExtraGlyph arrays must be sorted in ascending codepoint order to enable binary search.
 */
typedef struct
{
    uint32_t codepoint; /**< Unicode codepoint this entry covers. */
    GlyphInfo info; /**< Glyph metrics for this codepoint. */
} ExtraGlyph;

/**
 * @brief A baked bitmap font at a single pixel size.
 *
 * The dense glyph array covers the contiguous range [first, last].
 * Additional symbols outside that range are stored in the extra array.
 * The format field stores the bits-per-pixel of the bitmap data (ERUI_FONT_FMT_*
 * constants): 1 (packed 1-bit, MSB-first), 2 (2-bit grayscale, 4px/byte),
 * 4 (4-bit grayscale, 2px/byte), or 8 (8-bit grayscale, 1 byte/pixel).
 * A value of 0 is treated as 1-bit for backward compatibility.
 */
typedef struct
{
    const uint8_t* bitmap; /**< Font bitmap data; layout determined by format. */
    const GlyphInfo* glyphs; /**< Dense glyph array for codepoints [first, last]. */
    const ExtraGlyph* extras; /**< Sorted sparse glyph array for out-of-range codepoints. */
    uint16_t extras_count; /**< Number of entries in the extra array. */
    uint16_t first; /**< First codepoint covered by the dense glyphs array. */
    uint16_t last; /**< Last codepoint covered by the dense glyphs array. */
    uint8_t pixel_size; /**< Nominal font size in pixels (cap height). */
    uint8_t line_height; /**< Recommended line height including leading. */
    uint8_t baseline; /**< Distance from the top of the line box to the text baseline. */
    uint8_t format; /**< Bits per pixel: 1, 2, 4, or 8 (ERUI_FONT_FMT_* constants). 0 treated as 1-bit. */
} BitmapFont;

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Array of pointers to the built-in Inter Regular font at each baked size. */
extern const BitmapFont* const g_inter_sizes[];

/** @brief Number of entries in g_inter_sizes. */
extern const size_t g_inter_sizes_count;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Looks up the GlyphInfo for a Unicode codepoint in a BitmapFont.
 *
 * Searches the dense array first, then the sorted extras array.
 * Falls back to '?' or the first glyph if the codepoint is not found.
 *
 * @param[in] font       Font to search.
 * @param[in] codepoint  Unicode codepoint to look up.
 *
 * @return Pointer to the matching GlyphInfo (never NULL for a valid font).
 */
const GlyphInfo* font_glyph(const BitmapFont* font, uint32_t codepoint);

#endif
