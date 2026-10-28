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
 * volatile source, a copy of the pack) from the heap, then register through er_assets_load_pack_into. Kept
 * apart from er_assets.c so a firmware that only checks packs or loads them into caller storage never links
 * the allocator.
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
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

bool er_assets_load_pack(const void* buf, size_t len)
{
    return er_assets_load_pack_ex(buf, len, false);
}

bool er_assets_load_pack_ex(const void* buf, size_t len, bool copy)
{
    /* Check the whole pack, then take everything it needs, before registering anything: a pack that fails
     * leaves the registries (and the previous copy) as they were. */
    ErAssetsInfo info;
    if (!er_assets_check_pack(buf, len, &info))
    {
        return false;
    }

    /* One block per table for the whole pack, kept for the process (see er_assets.h). */
    ErAssetsStorage storage;
    memset(&storage, 0, sizeof(storage));
    storage.fonts = info.fonts ? (BitmapFont*)calloc(info.fonts, sizeof(BitmapFont)) : NULL;
    storage.glyphs = info.glyphs ? (GlyphInfo*)calloc(info.glyphs, sizeof(GlyphInfo)) : NULL;
    storage.extras = info.extra_glyphs ? (ExtraGlyph*)calloc(info.extra_glyphs, sizeof(ExtraGlyph)) : NULL;
    storage.font_count = info.fonts;
    storage.glyph_count = info.glyphs;
    storage.extra_count = info.extra_glyphs;
    bool ok =
        (!info.fonts || storage.fonts) && (!info.glyphs || storage.glyphs) && (!info.extra_glyphs || storage.extras);

    /* When the source is volatile (a hot-reload staging buffer about to be reused for the next upload),
     * take our own copy and register the engine against it, so the caller's buffer is free after load.
     * The default (copy=false) references @p buf in place — zero RAM cost from mmap'd flash — and the
     * caller keeps it live (production boot, desktop sim). */
    const uint8_t* src = (const uint8_t*)buf;
    if (ok && copy)
    {
        if (s_pack_copy_cap < len)
        {
            uint8_t* grown = (uint8_t*)realloc(s_pack_copy, len);
            ok = grown != NULL; /* OOM: prior copy left intact */
            if (grown)
            {
                s_pack_copy = grown;
                s_pack_copy_cap = len;
            }
        }
        if (ok)
        {
            memcpy(s_pack_copy, buf, len);
            src = s_pack_copy;
        }
    }

    if (!ok)
    {
        free(storage.fonts);
        free(storage.glyphs);
        free(storage.extras);
        return false;
    }
    return er_assets_load_pack_into(src, len, &storage);
}
