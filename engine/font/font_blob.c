#include "font_blob.h"
#include "font_registry.h"
#include <stdint.h>
#include <string.h>

#ifndef ERUI_FONT_POOL_BYTES
#define ERUI_FONT_POOL_BYTES 0
#endif

#if ERUI_FONT_POOL_BYTES > 0

/** @brief Maximum number of distinct (family, size) blobs whose pool footprint is tracked for reuse. */
#define FONT_BLOB_MAX_RECORDS (FONT_REGISTRY_MAX * FONT_FAMILY_MAX_SIZES)

static uint8_t s_pool[ERUI_FONT_POOL_BYTES];

/*----------------------------------------------------------------------------------------------------------------------
 - Types: Private
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Tracks the reserved pool footprint of one registered blob so a later
 *        re-registration of the same (family, size) can be repacked in place.
 */
typedef struct
{
    const BitmapFont* font; /**< Registered font whose pool region this describes. */
    uint32_t capacity;      /**< Total reserved bytes of the contiguous region at font. */
} BlobRecord;

#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Variables: Private
 ---------------------------------------------------------------------------------------------------------------------*/

static uint32_t s_used;
#if ERUI_FONT_POOL_BYTES > 0
static BlobRecord s_records[FONT_BLOB_MAX_RECORDS];
static uint16_t s_record_count;
#endif

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

#if ERUI_FONT_POOL_BYTES > 0

/**
 * @brief Rounds a byte count up to the next 4-byte boundary (matching blob_alloc()).
 *
 * @param[in] n  Unrounded byte count.
 *
 * @return n rounded up to a multiple of 4.
 */
static uint32_t align4(uint32_t n)
{
    return (n + 3U) & ~3U;
}

/**
 * @brief Finds the footprint record describing a previously registered font.
 *
 * @param[in] font  Font pointer to match.
 *
 * @return Pointer to the matching record, or NULL if none is tracked.
 */
static BlobRecord* find_record(const BitmapFont* font)
{
    for (uint16_t i = 0; i < s_record_count; i++)
    {
        if (s_records[i].font == font)
            return &s_records[i];
    }
    return NULL;
}

#endif

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

void font_blob_init(uint32_t reserve_offset)
{
#if ERUI_FONT_POOL_BYTES > 0
    s_record_count = 0;
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

    if (blob[0] != FONT_BLOB_MAGIC_0 || blob[1] != FONT_BLOB_MAGIC_1 || blob[2] != FONT_BLOB_MAGIC_2
        || blob[3] != FONT_BLOB_MAGIC_3)
        return FONT_BLOB_ERR_BAD_MAGIC;

    if (blob[4] != FONT_BLOB_VERSION)
        return FONT_BLOB_ERR_BAD_VERSION;

    const uint8_t first = blob[5];
    const uint8_t last = blob[6];
    const uint8_t pixel_size = blob[7];
    const uint8_t line_height = blob[8];
    const uint8_t baseline = blob[9];
    const uint8_t format = blob[10];
    const uint16_t glyph_count = read_u16_le(&blob[11]);
    const uint32_t bitmap_size = read_u32_le(&blob[13]);

    if (last < first || glyph_count != (uint16_t)(last - first + 1U))
        return FONT_BLOB_ERR_BAD_RANGE;

    /* Wire format packs each GlyphInfo as 8 bytes (bitmap_offset as uint16_t, no padding).
     * The in-memory GlyphInfo uses uint32_t for bitmap_offset + 3 pad bytes = 12 bytes.
     * Parse field-by-field, zero-extending the 2-byte wire offset to 4 bytes and zeroing pad. */
    const uint32_t glyph_wire_bytes = (uint32_t)glyph_count * 8U;
    const uint32_t glyph_mem_bytes = (uint32_t)glyph_count * sizeof(GlyphInfo);
    const uint32_t expected_size = FONT_BLOB_HEADER_SIZE + glyph_wire_bytes + bitmap_size;
    if (blob_size < expected_size)
        return FONT_BLOB_ERR_SIZE_MISMATCH;

    /* Reuse path: when re-registering the same (family, size) and the new blob fits within
     * the byte footprint reserved by the prior registration, repack into that same region
     * so no fresh pool bytes are consumed. Otherwise allocate fresh from the bump pool — in
     * which case the prior region's bytes are NOT reclaimed (see header documentation). */
    const uint32_t need_font = align4((uint32_t)sizeof(BitmapFont));
    const uint32_t need_glyph = align4(glyph_mem_bytes);
    const uint32_t need_bitmap = align4(bitmap_size);
    const uint32_t need_total = need_font + need_glyph + need_bitmap;

    const BitmapFont* existing = font_registry_get_exact(name, pixel_size);
    BlobRecord* rec = existing ? find_record(existing) : NULL;

    BitmapFont* font_dst;
    uint8_t* glyph_dst;
    uint8_t* bitmap_dst;
    bool reused = false;

    if (rec && rec->capacity >= need_total)
    {
        uint8_t* base = (uint8_t*)(uintptr_t)existing;
        font_dst = (BitmapFont*)(void*)base;
        glyph_dst = base + need_font;
        bitmap_dst = base + need_font + need_glyph;
        reused = true;
    }
    else
    {
        font_dst = (void*)blob_alloc(sizeof(BitmapFont));
        glyph_dst = blob_alloc(glyph_mem_bytes);
        bitmap_dst = blob_alloc(bitmap_size);
        if (!font_dst || !glyph_dst || (bitmap_size > 0U && !bitmap_dst))
            return FONT_BLOB_ERR_OUT_OF_MEMORY;
    }

    {
        const uint8_t* src = blob + FONT_BLOB_HEADER_SIZE;
        GlyphInfo* dst = (GlyphInfo*)(void*)glyph_dst;
        for (uint16_t i = 0; i < glyph_count; i++, src += 8U, dst++)
        {
            dst->bitmap_offset = read_u16_le(src);
            dst->width = src[2];
            dst->height = src[3];
            dst->x_offset = (int8_t)src[4];
            dst->y_offset = (int8_t)src[5];
            dst->advance = src[6];
            dst->_pad[0] = dst->_pad[1] = dst->_pad[2] = 0;
        }
    }
    if (bitmap_size > 0U)
        memcpy(bitmap_dst, blob + FONT_BLOB_HEADER_SIZE + glyph_wire_bytes, bitmap_size);

    font_dst->bitmap = bitmap_dst;
    font_dst->glyphs = (const GlyphInfo*)(void*)glyph_dst;
    font_dst->extras = NULL;
    font_dst->extras_count = 0;
    font_dst->pixel_size = pixel_size;
    font_dst->line_height = line_height;
    font_dst->baseline = baseline;
    font_dst->first = first;
    font_dst->last = last;
    font_dst->format = format;

    /* Update footprint tracking. On reuse the region (and its capacity) is unchanged. On a
     * fresh allocation, repoint an existing record to the new region (the old bytes leak) or
     * start tracking this font so a future re-register can repack in place. */
    if (!reused)
    {
        if (rec)
        {
            rec->font = font_dst;
            rec->capacity = need_total;
        }
        else if (s_record_count < FONT_BLOB_MAX_RECORDS)
        {
            s_records[s_record_count].font = font_dst;
            s_records[s_record_count].capacity = need_total;
            s_record_count++;
        }
    }

    if (!font_registry_add(name, font_dst))
        return FONT_BLOB_ERR_REGISTRY_FULL;

    return FONT_BLOB_OK;
#endif
}
