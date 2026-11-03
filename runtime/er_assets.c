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

/*
 * er_assets — portable ERPK asset-pack parser (see er_assets.h).
 *
 * Reads a pack from a caller-owned memory buffer and registers its images and fonts via the public
 * engine API (er_image_load / er_font_register). Image pixels and font bitmaps are referenced by
 * pointer into the buffer (the caller must keep it live); per-font GlyphInfo/ExtraGlyph arrays + the
 * BitmapFont structs are taken from caller storage here (er_assets_load_pack_into), or from the heap in
 * er_assets_heap.c.
 *
 * The pack is little-endian; multi-byte image pixels are read in place (works on LE hosts — desktop
 * x86, ESP32-S3, STM32H7).
 */

#include "er_assets_internal.h"

#include "er_scene.h" /* er_image_load, er_font_register, BitmapFont, GlyphInfo, ExtraGlyph */

#include <stdint.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Bounds-checked little-endian cursor
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief A read cursor over the pack buffer; `ok` clears on any over-read. */
typedef struct
{
    const uint8_t* next;
    const uint8_t* end;
    bool ok;
} Cur;

static uint8_t rd_u8(Cur* cursor)
{
    if (!cursor->ok || cursor->next + 1 > cursor->end)
    {
        cursor->ok = false;
        return 0;
    }
    return *cursor->next++;
}

static uint16_t rd_u16(Cur* cursor)
{
    if (!cursor->ok || cursor->next + 2 > cursor->end)
    {
        cursor->ok = false;
        return 0;
    }
    const uint16_t value = (uint16_t)(cursor->next[0] | (cursor->next[1] << 8));
    cursor->next += 2;
    return value;
}

static uint32_t rd_u32(Cur* cursor)
{
    if (!cursor->ok || cursor->next + 4 > cursor->end)
    {
        cursor->ok = false;
        return 0;
    }
    const uint32_t value = (uint32_t)cursor->next[0] | ((uint32_t)cursor->next[1] << 8)
                           | ((uint32_t)cursor->next[2] << 16) | ((uint32_t)cursor->next[3] << 24);
    cursor->next += 4;
    return value;
}

static int8_t rd_i8(Cur* cursor)
{
    return (int8_t)rd_u8(cursor);
}

/** @brief Returns a pointer to the next @p byte_count bytes and advances, or NULL on over-read. */
static const uint8_t* rd_bytes(Cur* cursor, size_t byte_count)
{
    if (!cursor->ok || cursor->next + byte_count > cursor->end)
    {
        cursor->ok = false;
        return NULL;
    }
    const uint8_t* bytes = cursor->next;
    cursor->next += byte_count;
    return bytes;
}

/** @brief Reads a u16-length-prefixed name into @p out (null-terminated, truncated to capacity). */
static void rd_name(Cur* cursor, char* out, size_t capacity)
{
    const uint16_t stored_length = rd_u16(cursor);
    const uint8_t* stored_name = rd_bytes(cursor, stored_length);
    size_t kept_length = (stored_length < capacity - 1) ? stored_length : capacity - 1;
    if (stored_name && cursor->ok)
    {
        memcpy(out, stored_name, kept_length);
    }
    else
    {
        kept_length = 0;
    }
    out[kept_length] = '\0';
}

/** @brief Fills a GlyphInfo from the 9-byte serialized form. */
static void rd_glyph(Cur* cursor, GlyphInfo* glyph)
{
    memset(glyph, 0, sizeof(*glyph));
    glyph->bitmap_offset = rd_u32(cursor);
    glyph->width = rd_u8(cursor);
    glyph->height = rd_u8(cursor);
    glyph->x_offset = rd_i8(cursor);
    glyph->y_offset = rd_i8(cursor);
    glyph->advance = rd_u8(cursor);
}

/*----------------------------------------------------------------------------------------------------------------------
 - The pack walk
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Longest name rd_name keeps, in bytes. */
#define IMAGE_NAME_READ_MAX 127u

/** @brief "ERPK" | version u32 | image_count u32 | font_count u32. */
#define PACK_HEADER_SIZE 16u
#define PACK_MAGIC_SIZE 4

/** @brief Version 1 has only ARGB8888 images; version 2 gives each image a pixel format. */
#define PACK_VERSION_ARGB8888 1u
#define PACK_VERSION_PIXEL_FORMATS 2u

/** @brief An image's pixel format, and its bytes per pixel. */
#define PIXEL_FORMAT_ARGB8888 0u
#define PIXEL_FORMAT_RGB565 1u
#define ARGB8888_BYTES 4u
#define RGB565_BYTES 2u

/** @brief Bytes one row of a glyph's bitmap takes in a font of @p format, or 0 for a format the engine can't draw. */
static uint32_t glyph_row_bytes(uint8_t format, uint32_t width)
{
    switch (format)
    {
        case ERUI_FONT_FMT_1BIT:
            return (width + 7u) / 8u;
        case ERUI_FONT_FMT_2BIT:
            return (width + 3u) / 4u;
        case ERUI_FONT_FMT_4BIT:
            return (width + 1u) / 2u;
        case ERUI_FONT_FMT_8BIT:
            return width;
        default:
            return 0u;
    }
}

/** @brief True when a glyph's bitmap lies inside its font's bitmap of @p bitmap_length bytes. */
static bool glyph_fits(const GlyphInfo* glyph, uint8_t format, uint32_t bitmap_length)
{
    if (glyph->width == 0u || glyph->height == 0u)
    {
        return true; /* nothing drawn */
    }
    const uint32_t bytes = glyph_row_bytes(format, glyph->width) * glyph->height;
    return glyph->bitmap_offset <= bitmap_length && bytes <= bitmap_length - glyph->bitmap_offset;
}

/** @brief The part of a family name the font registry compares: up to a NUL, at most ER_FONT_FAMILY_MAX bytes. */
static uint8_t family_name_length(const uint8_t* name, uint16_t length)
{
    uint8_t compared = 0u;
    while (compared < length && compared < ER_FONT_FAMILY_MAX && name[compared] != '\0')
    {
        compared++;
    }
    return compared;
}

/** @brief Counts one more size of the family named @p name, as the font registry would hold it. */
static void count_family(ErAssetsWalk* walk, const uint8_t* name, uint16_t length)
{
    const uint8_t name_length = family_name_length(name, length);
    const uint32_t known =
        walk->info.font_families < ER_ASSETS_MAX_FAMILIES ? walk->info.font_families : ER_ASSETS_MAX_FAMILIES;
    ErAssetsFamily* family = NULL;
    for (uint32_t family_index = 0; family_index < known; family_index++)
    {
        ErAssetsFamily* candidate = &walk->families[family_index];
        if (candidate->name_length == name_length && memcmp(candidate->name, name, name_length) == 0)
        {
            family = candidate;
            break;
        }
    }
    if (!family)
    {
        /* Past the table every new name still counts, so the total is never too low. */
        if (walk->info.font_families < ER_ASSETS_MAX_FAMILIES)
        {
            family = &walk->families[walk->info.font_families];
            family->name = name;
            family->name_length = name_length;
            family->sizes = 0u;
        }
        walk->info.font_families++;
    }
    /* Each size counts, even a repeat the registry would replace, so the count is never too low. */
    const uint32_t sizes = family ? ++family->sizes : 1u;
    if (sizes > walk->info.largest_family)
    {
        walk->info.largest_family = sizes;
    }
}

bool er_assets_walk(const uint8_t* source, size_t length, ErAssetsWalk* walk)
{
    if (!source || length < PACK_HEADER_SIZE)
    {
        return false;
    }
    Cur cursor = {source, source + length, true};

    const uint8_t* magic = rd_bytes(&cursor, PACK_MAGIC_SIZE);
    if (!magic || memcmp(magic, "ERPK", PACK_MAGIC_SIZE) != 0)
    {
        return false; /* not our pack (or truncated header) */
    }
    /* Version 2 adds a per-image format byte (0=ARGB8888, 1=RGB565) plus a pad that 4-byte
     * aligns the pixels; a pack with no RGB565 images is still emitted as version 1. */
    const uint32_t version = rd_u32(&cursor);
    if (version != PACK_VERSION_ARGB8888 && version != PACK_VERSION_PIXEL_FORMATS)
    {
        return false;
    }
    const uint32_t image_count = rd_u32(&cursor);
    const uint32_t font_count = rd_u32(&cursor);

    char name[IMAGE_NAME_READ_MAX + 1u];

    for (uint32_t image_index = 0; image_index < image_count && cursor.ok; image_index++)
    {
        const uint32_t name_record = (uint32_t)(cursor.next - source);
        rd_name(&cursor, name, sizeof(name));
        const uint32_t width = rd_u32(&cursor);
        const uint32_t height = rd_u32(&cursor);
        uint8_t format = PIXEL_FORMAT_ARGB8888; /* v1 packs are always ARGB8888 */
        if (version >= PACK_VERSION_PIXEL_FORMATS)
        {
            format = rd_u8(&cursor);
            if (format > PIXEL_FORMAT_RGB565)
            {
                return false; /* unknown pixel format: the record length is unknowable, abort */
            }
            rd_bytes(&cursor, (size_t)((4u - ((uintptr_t)(cursor.next - source) & 3u)) & 3u)); /* skip alignment pad */
        }
        /* Checked before multiplying, so a hostile size can't wrap the byte count. */
        const size_t bytes_per_pixel = format == PIXEL_FORMAT_RGB565 ? RGB565_BYTES : ARGB8888_BYTES;
        if (height != 0u && width > length / bytes_per_pixel / height)
        {
            return false;
        }
        const uint8_t* pixels = rd_bytes(&cursor, (size_t)width * height * bytes_per_pixel);
        if (cursor.ok && pixels)
        {
            walk->info.images++;
            walk->info.misaligned += ((uintptr_t)pixels & (bytes_per_pixel - 1u)) ? 1u : 0u;
            walk->info.empty_images += (width == 0u || height == 0u) ? 1u : 0u;
            walk->info.odd_rgb565 +=
                (format == PIXEL_FORMAT_RGB565 && width != 0u && height != 0u && ((uintptr_t)pixels & 1u)) ? 1u : 0u;
            if (walk->image_names && walk->info.images <= walk->image_capacity)
            {
                walk->image_names[walk->info.images - 1u] = name_record;
            }
            /* Both reference the pixels in the pack (the caller keeps it live). */
            if (walk->registering && format == PIXEL_FORMAT_RGB565)
                er_image_load_rgb565(name, pixels, (int)width, (int)height);
            else if (walk->registering)
                er_image_load(name, pixels, (int)width, (int)height);
        }
    }

    for (uint32_t font_index = 0; font_index < font_count && cursor.ok; font_index++)
    {
        const uint8_t* name_record = cursor.next;
        rd_name(&cursor, name, sizeof(name));
        BitmapFont header;
        memset(&header, 0, sizeof(header));
        header.pixel_size = rd_u8(&cursor);
        header.line_height = rd_u8(&cursor);
        header.baseline = rd_u8(&cursor);
        header.format = rd_u8(&cursor);
        header.first = rd_u16(&cursor);
        header.last = rd_u16(&cursor);
        const uint16_t glyph_count = rd_u16(&cursor);
        const uint16_t extras_count = rd_u16(&cursor);
        const uint32_t bitmap_length = rd_u32(&cursor);
        if (!cursor.ok)
        {
            break;
        }
        /* The engine draws only these formats, indexes glyphs[codepoint - first] for the whole range, and
         * falls back to glyphs[0]. */
        if (glyph_row_bytes(header.format, 1u) == 0u || header.first > header.last
            || glyph_count != (uint32_t)header.last - header.first + 1u)
        {
            return false;
        }
        walk->info.fonts++;
        walk->info.glyphs += glyph_count;
        walk->info.extra_glyphs += extras_count;
        const uint16_t name_length = (uint16_t)(name_record[0] | (name_record[1] << 8)); /* read by rd_name */
        count_family(walk, name_record + 2, name_length);

        BitmapFont* font = NULL;
        GlyphInfo* glyphs = NULL;
        ExtraGlyph* extras = NULL;
        if (walk->registering && !walk->tables(walk, glyph_count, extras_count, &font, &glyphs, &extras))
        {
            return false;
        }

        GlyphInfo glyph;
        for (uint16_t glyph_index = 0; glyph_index < glyph_count && cursor.ok; glyph_index++)
        {
            GlyphInfo* record = glyphs ? &glyphs[glyph_index] : &glyph;
            rd_glyph(&cursor, record);
            cursor.ok = cursor.ok && glyph_fits(record, header.format, bitmap_length);
        }
        uint32_t previous_codepoint = 0u;
        for (uint16_t extra_index = 0; extra_index < extras_count && cursor.ok; extra_index++)
        {
            /* Extras are binary searched, so their codepoints must climb. */
            const uint32_t codepoint = rd_u32(&cursor);
            GlyphInfo* record = extras ? &extras[extra_index].info : &glyph;
            rd_glyph(&cursor, record);
            cursor.ok = cursor.ok && glyph_fits(record, header.format, bitmap_length)
                        && (extra_index == 0u || codepoint > previous_codepoint);
            previous_codepoint = codepoint;
            if (extras)
            {
                extras[extra_index].codepoint = codepoint;
            }
        }
        const uint8_t* bitmap = rd_bytes(&cursor, bitmap_length);

        if (cursor.ok && font)
        {
            *font = header;
            font->glyphs = glyphs;
            font->extras = extras;
            font->extras_count = extras_count;
            font->bitmap = bitmap;
            er_font_register(name, font); /* tables kept; the bitmap points into the pack */
        }
        else if (font && walk->release)
        {
            walk->release(font, glyphs, extras);
        }
    }

    return cursor.ok;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Takes @p count entries of @p size bytes from a caller storage array, or NULL when it is out of room. */
static void* take(void* base, uint32_t capacity, uint32_t* used, size_t size, uint32_t count)
{
    if (!base || capacity - *used < count)
    {
        return NULL;
    }
    void* entries = (uint8_t*)base + (size_t)*used * size;
    *used += count;
    return entries;
}

static bool storage_tables(ErAssetsWalk* walk,
                           uint16_t glyph_count,
                           uint16_t extras_count,
                           BitmapFont** font,
                           GlyphInfo** glyphs,
                           ExtraGlyph** extras)
{
    ErAssetsStorage* storage = walk->storage;
    *font = (BitmapFont*)take(storage->fonts, storage->font_count, &storage->fonts_used, sizeof(BitmapFont), 1u);
    *glyphs =
        (GlyphInfo*)take(storage->glyphs, storage->glyph_count, &storage->glyphs_used, sizeof(GlyphInfo), glyph_count);
    *extras = (ExtraGlyph*)take(
        storage->extras, storage->extra_count, &storage->extras_used, sizeof(ExtraGlyph), extras_count);
    return *font && (glyph_count == 0u || *glyphs) && (extras_count == 0u || *extras);
}

bool er_assets_check_pack(const void* buffer, size_t length, ErAssetsInfo* info)
{
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    const bool ok = er_assets_walk((const uint8_t*)buffer, length, &walk);
    if (info)
    {
        *info = walk.info;
    }
    return ok;
}

bool er_assets_check_container(const ErContainer* container, ErAssetsInfo* info)
{
    /* One walk over every pack, so the counts and the families seen carry from one pack to the next. */
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    bool ok = container != NULL;
    for (uint32_t section_index = 0; ok && section_index < container->section_count; section_index++)
    {
        const ErContainerSection* section = &container->sections[section_index];
        if (section->type == ER_SECTION_ASSET_PACK)
        {
            ok = er_assets_walk(section->data, section->length, &walk);
        }
    }
    if (info)
    {
        *info = walk.info;
    }
    return ok;
}

/** @brief An image name in the pack as rd_name reads it: at most 127 bytes, up to a NUL. */
static void image_name(const uint8_t* pack, uint32_t name_record, const uint8_t** bytes, uint32_t* length)
{
    const uint8_t* record = pack + name_record;
    const uint32_t stored = (uint32_t)(record[0] | (record[1] << 8));
    const uint32_t kept = stored < IMAGE_NAME_READ_MAX ? stored : IMAGE_NAME_READ_MAX;
    *bytes = record + 2;
    *length = 0u;
    while (*length < kept && (*bytes)[*length] != '\0')
    {
        (*length)++;
    }
}

/** @brief Orders two names as strcmp does: by unsigned bytes, a prefix first. */
static int compare_names(const uint8_t* first, uint32_t first_length, const uint8_t* second, uint32_t second_length)
{
    const uint32_t shorter = first_length < second_length ? first_length : second_length;
    const int order = memcmp(first, second, shorter);
    if (order != 0)
    {
        return order;
    }
    return first_length < second_length ? -1 : first_length > second_length ? 1 : 0;
}

static int compare_image_names(const uint8_t* pack, uint32_t first_record, uint32_t second_record)
{
    const uint8_t* first;
    const uint8_t* second;
    uint32_t first_length;
    uint32_t second_length;
    image_name(pack, first_record, &first, &first_length);
    image_name(pack, second_record, &second, &second_length);
    return compare_names(first, first_length, second, second_length);
}

/** @brief Moves names[root] down until it is no smaller than its children (a heap of @p count names). */
static void sift_down(const uint8_t* pack, uint32_t* names, uint32_t root, uint32_t count)
{
    for (;;)
    {
        uint32_t largest = root;
        const uint32_t left_child = 2u * root + 1u;
        const uint32_t right_child = left_child + 1u;
        if (left_child < count && compare_image_names(pack, names[left_child], names[largest]) > 0)
        {
            largest = left_child;
        }
        if (right_child < count && compare_image_names(pack, names[right_child], names[largest]) > 0)
        {
            largest = right_child;
        }
        if (largest == root)
        {
            return;
        }
        const uint32_t swapped = names[root];
        names[root] = names[largest];
        names[largest] = swapped;
        root = largest;
    }
}

/** @brief Heapsort: no recursion, no allocation, O(n log n) on any input. */
static void sort_image_names(const uint8_t* pack, uint32_t* names, uint32_t count)
{
    for (uint32_t root = count / 2u; root-- > 0u;)
    {
        sift_down(pack, names, root, count);
    }
    for (uint32_t end = count; end-- > 1u;)
    {
        const uint32_t largest = names[0];
        names[0] = names[end];
        names[end] = largest;
        sift_down(pack, names, 0u, end);
    }
}

bool er_assets_check_pack_images(
    const void* buffer, size_t length, ErAssetsInfo* info, uint32_t* names, uint32_t capacity)
{
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.image_names = names;
    walk.image_capacity = names ? capacity : 0u;
    const bool ok = er_assets_walk((const uint8_t*)buffer, length, &walk);
    if (ok && names)
    {
        sort_image_names((const uint8_t*)buffer, names, walk.info.images < capacity ? walk.info.images : capacity);
    }
    if (info)
    {
        *info = walk.info;
    }
    return ok;
}

bool er_assets_find_image(const void* buffer, const uint32_t* names, uint32_t count, const char* name)
{
    if (!buffer || !names || !name)
    {
        return false;
    }
    const uint32_t name_length = (uint32_t)strlen(name);
    uint32_t low = 0u;
    uint32_t high = count;
    while (low < high)
    {
        const uint32_t middle = low + (high - low) / 2u;
        const uint8_t* candidate;
        uint32_t candidate_length;
        image_name((const uint8_t*)buffer, names[middle], &candidate, &candidate_length);
        const int order = compare_names(candidate, candidate_length, (const uint8_t*)name, name_length);
        if (order == 0)
        {
            return true;
        }
        if (order < 0)
        {
            low = middle + 1u;
        }
        else
        {
            high = middle;
        }
    }
    return false;
}

/** @brief True when a caller storage array of @p capacity entries at @p base has room for @p needed. */
static bool storage_fits(const void* base, uint32_t capacity, uint32_t needed)
{
    return needed == 0u || (base && needed <= capacity);
}

bool er_assets_load_pack_into(const void* buffer, size_t length, ErAssetsStorage* storage)
{
    if (!storage)
    {
        return false;
    }
    storage->fonts_used = 0u;
    storage->glyphs_used = 0u;
    storage->extras_used = 0u;
    /* Checked first, so a pack that fails registers nothing. */
    ErAssetsInfo info;
    if (!er_assets_check_pack(buffer, length, &info) || !storage_fits(storage->fonts, storage->font_count, info.fonts)
        || !storage_fits(storage->glyphs, storage->glyph_count, info.glyphs)
        || !storage_fits(storage->extras, storage->extra_count, info.extra_glyphs))
    {
        return false;
    }
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.registering = true;
    walk.tables = storage_tables;
    walk.storage = storage;
    return er_assets_walk((const uint8_t*)buffer, length, &walk);
}
