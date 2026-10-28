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
    const uint8_t* p;
    const uint8_t* end;
    bool ok;
} Cur;

static uint8_t rd_u8(Cur* c)
{
    if (!c->ok || c->p + 1 > c->end)
    {
        c->ok = false;
        return 0;
    }
    return *c->p++;
}

static uint16_t rd_u16(Cur* c)
{
    if (!c->ok || c->p + 2 > c->end)
    {
        c->ok = false;
        return 0;
    }
    const uint16_t v = (uint16_t)(c->p[0] | (c->p[1] << 8));
    c->p += 2;
    return v;
}

static uint32_t rd_u32(Cur* c)
{
    if (!c->ok || c->p + 4 > c->end)
    {
        c->ok = false;
        return 0;
    }
    const uint32_t v =
        (uint32_t)c->p[0] | ((uint32_t)c->p[1] << 8) | ((uint32_t)c->p[2] << 16) | ((uint32_t)c->p[3] << 24);
    c->p += 4;
    return v;
}

static int8_t rd_i8(Cur* c)
{
    return (int8_t)rd_u8(c);
}

/** @brief Returns a pointer to the next @p n bytes and advances, or NULL on over-read. */
static const uint8_t* rd_bytes(Cur* c, size_t n)
{
    if (!c->ok || c->p + n > c->end)
    {
        c->ok = false;
        return NULL;
    }
    const uint8_t* r = c->p;
    c->p += n;
    return r;
}

/** @brief Reads a u16-length-prefixed name into @p out (null-terminated, truncated to cap). */
static void rd_name(Cur* c, char* out, size_t cap)
{
    const uint16_t len = rd_u16(c);
    const uint8_t* s = rd_bytes(c, len);
    size_t n = (len < cap - 1) ? len : cap - 1;
    if (s && c->ok)
    {
        memcpy(out, s, n);
    }
    else
    {
        n = 0;
    }
    out[n] = '\0';
}

/** @brief Fills a GlyphInfo from the 9-byte serialized form. */
static void rd_glyph(Cur* c, GlyphInfo* g)
{
    memset(g, 0, sizeof(*g));
    g->bitmap_offset = rd_u32(c);
    g->width = rd_u8(c);
    g->height = rd_u8(c);
    g->x_offset = rd_i8(c);
    g->y_offset = rd_i8(c);
    g->advance = rd_u8(c);
}

/*----------------------------------------------------------------------------------------------------------------------
 - The pack walk
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief Longest name rd_name keeps, in bytes. */
#define IMAGE_NAME_READ_MAX 127u

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
    if (!source || length < 16)
    {
        return false;
    }
    Cur cursor = {source, source + length, true};

    const uint8_t* magic = rd_bytes(&cursor, 4);
    if (!magic || memcmp(magic, "ERPK", 4) != 0)
    {
        return false; /* not our pack (or truncated header) */
    }
    /* Version 2 adds a per-image format byte (0=ARGB8888, 1=RGB565) plus a pad that 4-byte
     * aligns the pixels; a pack with no RGB565 images is still emitted as version 1. */
    const uint32_t version = rd_u32(&cursor);
    if (version != 1u && version != 2u)
    {
        return false;
    }
    const uint32_t image_count = rd_u32(&cursor);
    const uint32_t font_count = rd_u32(&cursor);

    char name[IMAGE_NAME_READ_MAX + 1u];

    for (uint32_t image_index = 0; image_index < image_count && cursor.ok; image_index++)
    {
        const uint32_t name_record = (uint32_t)(cursor.p - source);
        rd_name(&cursor, name, sizeof(name));
        const uint32_t width = rd_u32(&cursor);
        const uint32_t height = rd_u32(&cursor);
        uint8_t format = 0u; /* v1 packs are always ARGB8888 */
        if (version >= 2u)
        {
            format = rd_u8(&cursor);
            if (format > 1u)
            {
                return false; /* unknown pixel format: the record length is unknowable, abort */
            }
            rd_bytes(&cursor, (size_t)((4u - ((uintptr_t)(cursor.p - source) & 3u)) & 3u)); /* skip alignment pad */
        }
        /* Checked before multiplying, so a hostile size can't wrap the byte count. */
        const size_t bytes_per_pixel = format == 1u ? 2u : 4u;
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
            if (walk->image_names && walk->info.images <= walk->image_capacity)
            {
                walk->image_names[walk->info.images - 1u] = name_record;
            }
            /* Both reference the pixels in the pack (the caller keeps it live). */
            if (walk->registering && format == 1u)
                er_image_load_rgb565(name, pixels, (int)width, (int)height);
            else if (walk->registering)
                er_image_load(name, pixels, (int)width, (int)height);
        }
    }

    for (uint32_t font_index = 0; font_index < font_count && cursor.ok; font_index++)
    {
        const uint8_t* name_record = cursor.p;
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

bool er_assets_check_pack(const void* buf, size_t len, ErAssetsInfo* info)
{
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    const bool ok = er_assets_walk((const uint8_t*)buf, len, &walk);
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

bool er_assets_check_pack_images(const void* buf, size_t len, ErAssetsInfo* info, uint32_t* names, uint32_t capacity)
{
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.image_names = names;
    walk.image_capacity = names ? capacity : 0u;
    const bool ok = er_assets_walk((const uint8_t*)buf, len, &walk);
    if (ok && names)
    {
        sort_image_names((const uint8_t*)buf, names, walk.info.images < capacity ? walk.info.images : capacity);
    }
    if (info)
    {
        *info = walk.info;
    }
    return ok;
}

bool er_assets_find_image(const void* buf, const uint32_t* names, uint32_t count, const char* name)
{
    if (!buf || !names || !name)
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
        image_name((const uint8_t*)buf, names[middle], &candidate, &candidate_length);
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

bool er_assets_load_pack_into(const void* buf, size_t len, ErAssetsStorage* storage)
{
    if (!storage)
    {
        return false;
    }
    storage->fonts_used = 0u;
    storage->glyphs_used = 0u;
    storage->extras_used = 0u;
    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.registering = true;
    walk.tables = storage_tables;
    walk.storage = storage;
    return er_assets_walk((const uint8_t*)buf, len, &walk);
}
