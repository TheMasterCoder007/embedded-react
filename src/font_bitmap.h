#ifndef EMBEDDED_REACT_FONT_BITMAP_H
#define EMBEDDED_REACT_FONT_BITMAP_H

#include <stddef.h>
#include <stdint.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Types
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Metrics and bitmap location for a single glyph.
 */
typedef struct
{
    uint16_t bitmap_offset; /**< Byte offset into the font bitmap array. */
    uint8_t  width;         /**< Glyph bitmap width in pixels. */
    uint8_t  height;        /**< Glyph bitmap height in pixels. */
    int8_t   x_offset;      /**< Horizontal bearing from the cursor origin to the glyph's left edge. */
    int8_t   y_offset;      /**< Vertical bearing from the cursor baseline to the glyph's top edge. */
    uint8_t  advance;       /**< Cursor advance in pixels after drawing this glyph. */
    uint8_t  _pad;          /**< Padding to maintain 8-byte struct alignment. */
} GlyphInfo;

/**
 * @brief A codepoint-to-glyph mapping entry for sparse (non-ASCII) glyphs.
 *
 * ExtraGlyph arrays must be sorted in ascending codepoint order to enable binary search.
 */
typedef struct
{
    uint32_t  codepoint; /**< Unicode codepoint this entry covers. */
    GlyphInfo info;      /**< Glyph metrics for this codepoint. */
} ExtraGlyph;

/**
 * @brief A baked bitmap font at a single pixel size.
 *
 * The dense glyph array covers the contiguous range [first, last].
 * Additional symbols outside that range are stored in the extra array.
 */
typedef struct
{
    const uint8_t*    bitmap;       /**< Packed 1-bit-per-pixel bitmap data. */
    const GlyphInfo*  glyphs;       /**< Dense glyph array for codepoints [first, last]. */
    const ExtraGlyph* extras;       /**< Sorted sparse glyph array for out-of-range codepoints. */
    uint16_t          extras_count; /**< Number of entries in the extra array. */
    uint16_t          first;        /**< First codepoint covered by the dense glyphs array. */
    uint16_t          last;         /**< Last codepoint covered by the dense glyphs array. */
    uint8_t           pixel_size;   /**< Nominal font size in pixels (cap height). */
    uint8_t           line_height;  /**< Recommended line height including leading. */
    uint8_t           baseline;     /**< Distance from the top of the line box to the text baseline. */
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
