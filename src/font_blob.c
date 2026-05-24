#include "font_blob.h"
#include "font_registry.h"
#include <string.h>

#ifndef ERUI_FONT_POOL_BYTES
#define ERUI_FONT_POOL_BYTES 0
#endif

#if ERUI_FONT_POOL_BYTES > 0
static uint8_t s_pool[ERUI_FONT_POOL_BYTES];
#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_used;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Allocates a 4-byte-aligned block from the static font pool.
 *
 * Returns NULL if ERUI_FONT_POOL_BYTES is zero (pool disabled) or if there
 * is insufficient remaining space.
 *
 * @param[in] bytes  Number of bytes to allocate.
 *
 * @return Pointer to the allocated block, or NULL on failure.
 */
static uint8_t* blob_alloc(uint32_t bytes)
{
#if ERUI_FONT_POOL_BYTES > 0
    const uint32_t aligned = (bytes + 3U) & ~3U;
    if ((uint64_t)s_used + aligned > (uint64_t)ERUI_FONT_POOL_BYTES)
        return NULL;
    uint8_t* ptr = s_pool + s_used;
    s_used += aligned;
    return ptr;
#else
    (void)bytes;
    return NULL;
#endif
}

/**
 * @brief Reads a little-endian uint16_t from an unaligned byte pointer.
 *
 * @param[in] p  Pointer to the first byte of the 16-bit value.
 *
 * @return The decoded uint16_t value.
 */
static uint16_t read_u16_le(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/**
 * @brief Reads a little-endian uint32_t from an unaligned byte pointer.
 *
 * @param[in] p  Pointer to the first byte of the 32-bit value.
 *
 * @return The decoded uint32_t value.
 */
static uint32_t read_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void font_blob_init(uint32_t reserve_offset)
{
#if ERUI_FONT_POOL_BYTES > 0
    const uint32_t aligned = (reserve_offset + 3U) & ~3U;
    s_used = (aligned <= ERUI_FONT_POOL_BYTES) ? aligned : ERUI_FONT_POOL_BYTES;
#else
    (void)reserve_offset;
    s_used = 0;
#endif
}

uint32_t font_blob_used_bytes(void)
{
    return s_used;
}

FontBlobStatus font_blob_register(const char* name, const uint8_t* blob, uint32_t blob_size)
{
#if ERUI_FONT_POOL_BYTES <= 0
    (void)name;
    (void)blob;
    (void)blob_size;
    return FONT_BLOB_ERR_OUT_OF_MEMORY;
#else
    if (!name || !blob || blob_size < FONT_BLOB_HEADER_SIZE)
        return FONT_BLOB_ERR_TOO_SHORT;

    if (blob[0] != FONT_BLOB_MAGIC_0 || blob[1] != FONT_BLOB_MAGIC_1 || blob[2] != FONT_BLOB_MAGIC_2 ||
        blob[3] != FONT_BLOB_MAGIC_3)
        return FONT_BLOB_ERR_BAD_MAGIC;

    if (blob[4] != FONT_BLOB_VERSION)
        return FONT_BLOB_ERR_BAD_VERSION;

    const uint8_t first = blob[5];
    const uint8_t last = blob[6];
    const uint8_t pixel_size = blob[7];
    const uint8_t line_height = blob[8];
    const uint8_t baseline = blob[9];
    const uint16_t glyph_count = read_u16_le(&blob[10]);
    const uint32_t bitmap_size = read_u32_le(&blob[12]);

    if (last < first || glyph_count != (uint16_t)(last - first + 1U))
        return FONT_BLOB_ERR_BAD_RANGE;

    const uint32_t glyph_bytes = (uint32_t)glyph_count * sizeof(GlyphInfo);
    const uint32_t expected_size = FONT_BLOB_HEADER_SIZE + glyph_bytes + bitmap_size;
    if (blob_size < expected_size)
        return FONT_BLOB_ERR_SIZE_MISMATCH;

    BitmapFont* font_dst = (void*)blob_alloc(sizeof(BitmapFont));
    uint8_t* glyph_dst = blob_alloc(glyph_bytes);
    uint8_t* bitmap_dst = blob_alloc(bitmap_size);
    if (!font_dst || !glyph_dst || (bitmap_size > 0U && !bitmap_dst))
        return FONT_BLOB_ERR_OUT_OF_MEMORY;

    memcpy(glyph_dst, blob + FONT_BLOB_HEADER_SIZE, glyph_bytes);
    if (bitmap_size > 0U)
        memcpy(bitmap_dst, blob + FONT_BLOB_HEADER_SIZE + glyph_bytes, bitmap_size);

    font_dst->bitmap = bitmap_dst;
    font_dst->glyphs = (const GlyphInfo*)(void*)glyph_dst;
    font_dst->extras = NULL;
    font_dst->extras_count = 0;
    font_dst->pixel_size = pixel_size;
    font_dst->line_height = line_height;
    font_dst->baseline = baseline;
    font_dst->first = first;
    font_dst->last = last;

    if (!font_registry_add(name, font_dst))
        return FONT_BLOB_ERR_REGISTRY_FULL;

    return FONT_BLOB_OK;
#endif
}
