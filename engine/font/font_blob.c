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

#include "font_blob.h"
#include "font_registry.h"
#include <stdint.h>
#include <string.h>

#ifndef ERUI_FONT_POOL_BYTES
#define ERUI_FONT_POOL_BYTES 0
#endif

/** @brief Pool blocks are 4-byte aligned: this mask rounds a size up to the next boundary. */
#define FONT_BLOB_ALIGN_MASK 3U

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
    const uint32_t aligned = (bytes + FONT_BLOB_ALIGN_MASK) & ~FONT_BLOB_ALIGN_MASK;
    if ((uint64_t)s_used + aligned > (uint64_t)ERUI_FONT_POOL_BYTES)
        return NULL;
    uint8_t* block = s_pool + s_used;
    s_used += aligned;
    return block;
#else
    (void)bytes;
    return NULL;
#endif
}

/**
 * @brief Reads a little-endian uint16_t from an unaligned byte pointer.
 *
 * @param[in] bytes  Pointer to the first byte of the 16-bit value.
 *
 * @return The decoded uint16_t value.
 */
static uint16_t read_u16_le(const uint8_t* bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

/**
 * @brief Reads a little-endian uint32_t from an unaligned byte pointer.
 *
 * @param[in] bytes  Pointer to the first byte of the 32-bit value.
 *
 * @return The decoded uint32_t value.
 */
static uint32_t read_u32_le(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

#if ERUI_FONT_POOL_BYTES > 0

/**
 * @brief Rounds a byte count up to the next 4-byte boundary (matching blob_alloc()).
 *
 * @param[in] byte_count  Unrounded byte count.
 *
 * @return byte_count rounded up to a multiple of 4.
 */
static uint32_t align4(uint32_t byte_count)
{
    return (byte_count + FONT_BLOB_ALIGN_MASK) & ~FONT_BLOB_ALIGN_MASK;
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
    for (uint16_t record_index = 0; record_index < s_record_count; record_index++)
    {
        if (s_records[record_index].font == font)
            return &s_records[record_index];
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
    const uint32_t aligned = (reserve_offset + FONT_BLOB_ALIGN_MASK) & ~FONT_BLOB_ALIGN_MASK;
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

    if (blob[FONT_BLOB_VERSION_AT] != FONT_BLOB_VERSION)
        return FONT_BLOB_ERR_BAD_VERSION;

    const uint8_t first = blob[FONT_BLOB_FIRST_AT];
    const uint8_t last = blob[FONT_BLOB_LAST_AT];
    const uint8_t pixel_size = blob[FONT_BLOB_PIXEL_SIZE_AT];
    const uint8_t line_height = blob[FONT_BLOB_LINE_HEIGHT_AT];
    const uint8_t baseline = blob[FONT_BLOB_BASELINE_AT];
    const uint8_t format = blob[FONT_BLOB_FORMAT_AT];
    const uint16_t glyph_count = read_u16_le(&blob[FONT_BLOB_GLYPH_COUNT_AT]);
    const uint32_t bitmap_size = read_u32_le(&blob[FONT_BLOB_BITMAP_SIZE_AT]);

    if (last < first || glyph_count != (uint16_t)(last - first + 1U))
        return FONT_BLOB_ERR_BAD_RANGE;

    /* Wire format packs each GlyphInfo as 8 bytes (bitmap_offset as uint16_t, no padding).
     * The in-memory GlyphInfo uses uint32_t for bitmap_offset + 3 pad bytes = 12 bytes.
     * Parse field-by-field, zero-extending the 2-byte wire offset to 4 bytes and zeroing pad. */
    const uint32_t glyph_wire_bytes = (uint32_t)glyph_count * FONT_BLOB_GLYPH_BYTES;
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
    BlobRecord* record = existing ? find_record(existing) : NULL;

    BitmapFont* font_destination;
    uint8_t* glyph_destination;
    uint8_t* bitmap_destination;
    bool reused = false;

    if (record && record->capacity >= need_total)
    {
        uint8_t* base = (uint8_t*)(uintptr_t)existing;
        font_destination = (BitmapFont*)(void*)base;
        glyph_destination = base + need_font;
        bitmap_destination = base + need_font + need_glyph;
        reused = true;
    }
    else
    {
        font_destination = (void*)blob_alloc(sizeof(BitmapFont));
        glyph_destination = blob_alloc(glyph_mem_bytes);
        bitmap_destination = blob_alloc(bitmap_size);
        if (!font_destination || !glyph_destination || (bitmap_size > 0U && !bitmap_destination))
            return FONT_BLOB_ERR_OUT_OF_MEMORY;
    }

    {
        const uint8_t* wire_glyph = blob + FONT_BLOB_HEADER_SIZE;
        GlyphInfo* glyph = (GlyphInfo*)(void*)glyph_destination;
        for (uint16_t glyph_index = 0; glyph_index < glyph_count;
             glyph_index++, wire_glyph += FONT_BLOB_GLYPH_BYTES, glyph++)
        {
            glyph->bitmap_offset = read_u16_le(wire_glyph);
            glyph->width = wire_glyph[FONT_BLOB_GLYPH_WIDTH_AT];
            glyph->height = wire_glyph[FONT_BLOB_GLYPH_HEIGHT_AT];
            glyph->x_offset = (int8_t)wire_glyph[FONT_BLOB_GLYPH_X_OFFSET_AT];
            glyph->y_offset = (int8_t)wire_glyph[FONT_BLOB_GLYPH_Y_OFFSET_AT];
            glyph->advance = wire_glyph[FONT_BLOB_GLYPH_ADVANCE_AT];
            glyph->_pad[0] = glyph->_pad[1] = glyph->_pad[2] = 0;
        }
    }
    if (bitmap_size > 0U)
        memcpy(bitmap_destination, blob + FONT_BLOB_HEADER_SIZE + glyph_wire_bytes, bitmap_size);

    font_destination->bitmap = bitmap_destination;
    font_destination->glyphs = (const GlyphInfo*)(void*)glyph_destination;
    font_destination->extras = NULL;
    font_destination->extras_count = 0;
    font_destination->pixel_size = pixel_size;
    font_destination->line_height = line_height;
    font_destination->baseline = baseline;
    font_destination->first = first;
    font_destination->last = last;
    font_destination->format = format;

    /* Update footprint tracking. On reuse the region (and its capacity) is unchanged. On a
     * fresh allocation, repoint an existing record to the new region (the old bytes leak) or
     * start tracking this font so a future re-register can repack in place. */
    if (!reused)
    {
        if (record)
        {
            record->font = font_destination;
            record->capacity = need_total;
        }
        else if (s_record_count < FONT_BLOB_MAX_RECORDS)
        {
            s_records[s_record_count].font = font_destination;
            s_records[s_record_count].capacity = need_total;
            s_record_count++;
        }
    }

    if (!font_registry_add(name, font_destination))
        return FONT_BLOB_ERR_REGISTRY_FULL;

    return FONT_BLOB_OK;
#endif
}
