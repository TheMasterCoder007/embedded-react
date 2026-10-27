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
 * BitmapFont structs are heap-allocated and kept for the process, or taken from caller storage
 * (er_assets_load_pack_into).
 *
 * The pack is little-endian; multi-byte image pixels are read in place (works on LE hosts — desktop
 * x86, ESP32-S3, STM32H7).
 */

#include "er_assets.h"

#include "er_scene.h" /* er_image_load, er_font_register, BitmapFont, GlyphInfo, ExtraGlyph */

#include <stdint.h>
#include <stdlib.h>
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
 - Optional own-the-bytes copy (for volatile sources — see er_assets_load_pack_ex)
 ---------------------------------------------------------------------------------------------------------------------*/

/* A persistent copy of the pack, used only when a caller asks for it (copy=true). It is reused (grown as
 * needed) across loads, so repeated hot reloads don't leak a block each time. NULL/unused in the default
 * zero-copy path, so production (mmap'd flash) and the desktop sim spend nothing on it. */
static uint8_t* s_pack_copy = NULL;
static size_t s_pack_copy_cap = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------------------------------
 - The pack walk
 ---------------------------------------------------------------------------------------------------------------------*/

/** @brief What a walk does with each image and font it reads. */
typedef enum
{
    WALK_CHECK, /**< Count and check only; register nothing. */
    WALK_HEAP,  /**< Register, with font tables from the heap. */
    WALK_STORE, /**< Register, with font tables from caller storage. */
} WalkMode;

/** @brief One walk over a pack. */
typedef struct
{
    WalkMode mode;
    ErAssetsStorage* storage; /**< WALK_STORE: where font tables go. */
    ErAssetsInfo info;        /**< Counts so far. */
    const char* find;         /**< Non-NULL: an image name to look for. */
    bool found;               /**< The image named by find was seen. */
} Walk;

/**
 * @brief Takes @p count entries from caller storage, or NULL when it is out of room.
 *
 * @param[in]     base   Storage array.
 * @param[in]     cap    Its length.
 * @param[in,out] used   Entries already taken.
 * @param[in]     size   Bytes per entry.
 * @param[in]     count  Entries wanted.
 */
static void* take(void* base, uint32_t cap, uint32_t* used, size_t size, uint32_t count)
{
    if (!base || cap - *used < count)
    {
        return NULL;
    }
    void* entries = (uint8_t*)base + (size_t)*used * size;
    *used += count;
    return entries;
}

/**
 * @brief Walks a pack: checks every record bound and, unless only checking, registers each image and font.
 *
 * @return true when the whole pack was read cleanly (and, when storing, every font table fitted).
 */
static bool walk_pack(const uint8_t* src, size_t len, Walk* walk)
{
    if (!src || len < 16)
    {
        return false;
    }
    Cur c = {src, src + len, true};

    const uint8_t* magic = rd_bytes(&c, 4);
    if (!magic || memcmp(magic, "ERPK", 4) != 0)
    {
        return false; /* not our pack (or truncated header) */
    }
    /* Version 2 adds a per-image format byte (0=ARGB8888, 1=RGB565) plus a pad that 4-byte
     * aligns the pixels; a pack with no RGB565 images is still emitted as version 1. */
    const uint32_t version = rd_u32(&c);
    if (version != 1u && version != 2u)
    {
        return false;
    }
    const uint32_t n_images = rd_u32(&c);
    const uint32_t n_fonts = rd_u32(&c);
    const bool registering = walk->mode != WALK_CHECK;

    char name[128];

    for (uint32_t i = 0; i < n_images && c.ok; i++)
    {
        rd_name(&c, name, sizeof(name));
        const uint32_t w = rd_u32(&c);
        const uint32_t h = rd_u32(&c);
        uint8_t fmt = 0u; /* v1 packs are always ARGB8888 */
        if (version >= 2u)
        {
            fmt = rd_u8(&c);
            if (fmt > 1u)
            {
                return false; /* unknown pixel format: the record length is unknowable, abort */
            }
            rd_bytes(&c, (size_t)((4u - ((uintptr_t)(c.p - src) & 3u)) & 3u)); /* skip alignment pad */
        }
        /* Checked before multiplying, so a hostile size can't wrap the byte count. */
        const size_t bytes_per_pixel = fmt == 1u ? 2u : 4u;
        if (h != 0u && w > len / bytes_per_pixel / h)
        {
            return false;
        }
        const uint8_t* px = rd_bytes(&c, (size_t)w * h * bytes_per_pixel);
        if (c.ok && px)
        {
            walk->info.images++;
            walk->info.misaligned += ((uintptr_t)px & (bytes_per_pixel - 1u)) ? 1u : 0u;
            if (walk->find && strcmp(walk->find, name) == 0)
            {
                walk->found = true;
            }
            /* Both reference px in `buf` (caller keeps it live). */
            if (registering && fmt == 1u)
                er_image_load_rgb565(name, px, (int)w, (int)h);
            else if (registering)
                er_image_load(name, px, (int)w, (int)h);
        }
    }

    for (uint32_t i = 0; i < n_fonts && c.ok; i++)
    {
        rd_name(&c, name, sizeof(name));
        BitmapFont header;
        memset(&header, 0, sizeof(header));
        header.pixel_size = rd_u8(&c);
        header.line_height = rd_u8(&c);
        header.baseline = rd_u8(&c);
        header.format = rd_u8(&c);
        header.first = rd_u16(&c);
        header.last = rd_u16(&c);
        const uint16_t glyph_count = rd_u16(&c);
        const uint16_t extras_count = rd_u16(&c);
        const uint32_t bitmap_len = rd_u32(&c);
        if (!c.ok)
        {
            break;
        }
        walk->info.fonts++;
        walk->info.glyphs += glyph_count;
        walk->info.extra_glyphs += extras_count;

        BitmapFont* bf = NULL;
        GlyphInfo* glyphs = NULL;
        ExtraGlyph* extras = NULL;
        if (walk->mode == WALK_HEAP)
        {
            bf = (BitmapFont*)calloc(1, sizeof(BitmapFont));
            glyphs = (GlyphInfo*)calloc(glyph_count ? glyph_count : 1, sizeof(GlyphInfo));
            extras = extras_count ? (ExtraGlyph*)calloc(extras_count, sizeof(ExtraGlyph)) : NULL;
            if (!bf || !glyphs || (extras_count && !extras))
            {
                free(bf);
                free(glyphs);
                free(extras);
                return false;
            }
        }
        else if (walk->mode == WALK_STORE)
        {
            ErAssetsStorage* storage = walk->storage;
            bf = (BitmapFont*)take(storage->fonts, storage->font_count, &storage->fonts_used, sizeof(BitmapFont), 1u);
            glyphs = (GlyphInfo*)take(
                storage->glyphs, storage->glyph_count, &storage->glyphs_used, sizeof(GlyphInfo), glyph_count);
            extras = (ExtraGlyph*)take(
                storage->extras, storage->extra_count, &storage->extras_used, sizeof(ExtraGlyph), extras_count);
            if (!bf || (glyph_count && !glyphs) || (extras_count && !extras))
            {
                return false;
            }
        }

        GlyphInfo glyph;
        for (uint16_t g = 0; g < glyph_count; g++)
        {
            rd_glyph(&c, glyphs ? &glyphs[g] : &glyph);
        }
        for (uint16_t e = 0; e < extras_count; e++)
        {
            const uint32_t codepoint = rd_u32(&c);
            rd_glyph(&c, extras ? &extras[e].info : &glyph);
            if (extras)
            {
                extras[e].codepoint = codepoint;
            }
        }
        const uint8_t* bitmap = rd_bytes(&c, bitmap_len);

        if (c.ok && bf)
        {
            *bf = header;
            bf->glyphs = glyphs;
            bf->extras = extras;
            bf->extras_count = extras_count;
            bf->bitmap = bitmap;
            er_font_register(name, bf); /* glyphs/extras/bf kept; bitmap points into `buf` */
        }
        else if (walk->mode == WALK_HEAP)
        {
            free(bf);
            free(glyphs);
            free(extras);
        }
    }

    return c.ok;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_assets_load_pack(const void* buf, size_t len)
{
    return er_assets_load_pack_ex(buf, len, false);
}

bool er_assets_load_pack_ex(const void* buf, size_t len, bool copy)
{
    if (!buf || len < 16)
    {
        return false;
    }

    /* When the source is volatile (a hot-reload staging buffer about to be reused for the next upload),
     * take our own copy and register the engine against it, so the caller's buffer is free after load.
     * The default (copy=false) references @p buf in place — zero RAM cost from mmap'd flash — and the
     * caller keeps it live (production boot, desktop sim). */
    const uint8_t* src = (const uint8_t*)buf;
    if (copy)
    {
        if (s_pack_copy_cap < len)
        {
            uint8_t* grown = (uint8_t*)realloc(s_pack_copy, len);
            if (!grown)
            {
                return false; /* OOM: prior copy left intact; the load fails cleanly */
            }
            s_pack_copy = grown;
            s_pack_copy_cap = len;
        }
        memcpy(s_pack_copy, buf, len);
        src = s_pack_copy;
    }

    Walk walk = {WALK_HEAP, NULL, {0u, 0u, 0u, 0u, 0u}, NULL, false};
    return walk_pack(src, len, &walk);
}

bool er_assets_check_pack(const void* buf, size_t len, ErAssetsInfo* info)
{
    Walk walk = {WALK_CHECK, NULL, {0u, 0u, 0u, 0u, 0u}, NULL, false};
    const bool ok = walk_pack((const uint8_t*)buf, len, &walk);
    if (info)
    {
        *info = walk.info;
    }
    return ok;
}

bool er_assets_pack_has_image(const void* buf, size_t len, const char* name)
{
    Walk walk = {WALK_CHECK, NULL, {0u, 0u, 0u, 0u, 0u}, name, false};
    return name && walk_pack((const uint8_t*)buf, len, &walk) && walk.found;
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
    Walk walk = {WALK_STORE, storage, {0u, 0u, 0u, 0u, 0u}, NULL, false};
    return walk_pack((const uint8_t*)buf, len, &walk);
}
