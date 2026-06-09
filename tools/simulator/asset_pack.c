/*
 * Simulator asset-pack loader (see asset_pack.h): reads an ERPK pack file into RAM and hands it to the
 * portable parser (er_assets_load_pack, in the bridge). The buffer is kept alive for the process
 * (image pixels + font bitmaps reference it); a reload allocates a fresh one and leaks the previous —
 * fine for a dev tool. Parsing + registration live in bridges/quickjs/er_assets.c so the same code
 * loads a pack from flash on a device (and from a config container).
 */

#include "asset_pack.h"

#include "er_assets.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
    uint8_t* buf = (uint8_t*)malloc((size_t)size); /* kept alive: er_assets_load_pack references it */
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
    if (!er_assets_load_pack(buf, (size_t)size))
    {
        free(buf); /* malformed / partially written — keep whatever was registered before */
        return false;
    }
    return true;
}
