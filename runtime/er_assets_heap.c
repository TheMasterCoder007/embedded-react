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
 * er_assets_heap — er_assets_load_pack and er_assets_load_pack_ex, which take font tables (and, for a
 * volatile source, a copy of the pack) from the heap. Kept apart from er_assets.c so a firmware that only
 * checks packs or loads them into caller storage never links the allocator.
 */

#include "er_assets_internal.h"

#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Optional own-the-bytes copy (for volatile sources — see er_assets_load_pack_ex)
 ---------------------------------------------------------------------------------------------------------------------*/

/* A persistent copy of the pack, used only when a caller asks for it (copy=true). It is reused (grown as
 * needed) across loads, so repeated hot reloads don't leak a block each time. NULL/unused in the default
 * zero-copy path, so production (mmap'd flash) and the desktop sim spend nothing on it. */
static uint8_t* s_pack_copy = NULL;
static size_t s_pack_copy_cap = 0;

/*----------------------------------------------------------------------------------------------------------------------
 - Heap font tables
 ---------------------------------------------------------------------------------------------------------------------*/

static void heap_release(BitmapFont* font, GlyphInfo* glyphs, ExtraGlyph* extras)
{
    free(font);
    free(glyphs);
    free(extras);
}

static bool heap_tables(ErAssetsWalk* walk,
                        uint16_t glyph_count,
                        uint16_t extras_count,
                        BitmapFont** font,
                        GlyphInfo** glyphs,
                        ExtraGlyph** extras)
{
    (void)walk;
    *font = (BitmapFont*)calloc(1, sizeof(BitmapFont));
    *glyphs = (GlyphInfo*)calloc(glyph_count ? glyph_count : 1, sizeof(GlyphInfo));
    *extras = extras_count ? (ExtraGlyph*)calloc(extras_count, sizeof(ExtraGlyph)) : NULL;
    if (!*font || !*glyphs || (extras_count && !*extras))
    {
        heap_release(*font, *glyphs, *extras);
        *font = NULL;
        return false;
    }
    return true;
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

    ErAssetsWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.registering = true;
    walk.tables = heap_tables;
    walk.release = heap_release;
    return er_assets_walk(src, len, &walk);
}
