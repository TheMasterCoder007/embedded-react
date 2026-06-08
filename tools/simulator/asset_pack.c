/*
 * Runtime ERPK asset-pack loader for the simulator (see /SIMULATOR.md, Phase 2b + asset_pack.h).
 *
 * Parses the binary pack emitted by bridges/quickjs/js/assets/emit-pack.mjs and registers its images
 * and fonts via the public engine API (er_image_load / er_font_register). Image pixels and font
 * bitmaps are referenced by pointer into the loaded file buffer; per-font GlyphInfo/ExtraGlyph arrays
 * and the BitmapFont structs are heap-allocated. All allocations are kept alive for the process — a
 * reload allocates a fresh set and leaks the previous one rather than risk freeing memory the engine
 * still references (the engine has no "unregister" call). That tiny leak is fine for a dev tool.
 *
 * The pack is little-endian; the simulator is x86, so multi-byte image pixels are read in place.
 */

#include "asset_pack.h"

#include "er_scene.h" /* er_image_load, er_font_register, BitmapFont, GlyphInfo, ExtraGlyph */

#include <stdio.h>
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
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_asset_pack_load(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        return false;
    }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf)
    {
        fclose(f);
        return false;
    }
    const size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size)
    {
        free(buf);
        return false;
    }

    Cur c = {buf, buf + size, true};
    const uint8_t* magic = rd_bytes(&c, 4);
    if (!magic || memcmp(magic, "ERPK", 4) != 0 || rd_u32(&c) != 1u)
    {
        free(buf); /* not our pack (or truncated header) */
        return false;
    }
    const uint32_t n_images = rd_u32(&c);
    const uint32_t n_fonts = rd_u32(&c);

    char name[128];

    for (uint32_t i = 0; i < n_images && c.ok; i++)
    {
        rd_name(&c, name, sizeof(name));
        const uint32_t w = rd_u32(&c);
        const uint32_t h = rd_u32(&c);
        const uint8_t* px = rd_bytes(&c, (size_t)w * h * 4u);
        if (c.ok && px)
        {
            er_image_load(name, px, (int)w, (int)h); /* references px in `buf` (kept alive) */
        }
    }

    for (uint32_t i = 0; i < n_fonts && c.ok; i++)
    {
        rd_name(&c, name, sizeof(name));
        BitmapFont* bf = (BitmapFont*)calloc(1, sizeof(BitmapFont));
        bf->pixel_size = rd_u8(&c);
        bf->line_height = rd_u8(&c);
        bf->baseline = rd_u8(&c);
        bf->format = rd_u8(&c);
        bf->first = rd_u16(&c);
        bf->last = rd_u16(&c);
        const uint16_t glyph_count = rd_u16(&c);
        const uint16_t extras_count = rd_u16(&c);
        const uint32_t bitmap_len = rd_u32(&c);

        GlyphInfo* glyphs = (GlyphInfo*)calloc(glyph_count ? glyph_count : 1, sizeof(GlyphInfo));
        for (uint16_t g = 0; g < glyph_count; g++)
        {
            rd_glyph(&c, &glyphs[g]);
        }
        ExtraGlyph* extras = NULL;
        if (extras_count)
        {
            extras = (ExtraGlyph*)calloc(extras_count, sizeof(ExtraGlyph));
            for (uint16_t e = 0; e < extras_count; e++)
            {
                extras[e].codepoint = rd_u32(&c);
                rd_glyph(&c, &extras[e].info);
            }
        }
        const uint8_t* bitmap = rd_bytes(&c, bitmap_len);

        if (c.ok)
        {
            bf->glyphs = glyphs;
            bf->extras = extras;
            bf->extras_count = extras_count;
            bf->bitmap = bitmap;
            er_font_register(name, bf); /* glyphs/extras/bf leaked on reload; bitmap is in `buf` */
        }
        else
        {
            free(bf);
            free(glyphs);
            free(extras);
        }
    }

    if (!c.ok)
    {
        free(buf); /* malformed / partially written — keep whatever was registered before */
        return false;
    }
    /* `buf` is intentionally kept alive (image pixels + font bitmaps point into it). */
    return true;
}
