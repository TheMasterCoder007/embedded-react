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
 * Simulator asset-pack loader (see asset_pack.h): reads an ERPK pack file into RAM and hands it to the
 * portable parser (er_assets_load_pack, in runtime/). The buffer is kept alive for the process
 * (image pixels + font bitmaps reference it); a reload allocates a fresh one and leaks the previous —
 * fine for a dev tool. Parsing + registration live in runtime/er_assets.c so the same code
 * loads a pack from flash on a device (and from a config container).
 */

#include "asset_pack.h"

#include "er_assets.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

bool er_asset_pack_load(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(file);
        return false;
    }
    uint8_t* buffer = (uint8_t*)malloc((size_t)size); /* kept alive: er_assets_load_pack references it */
    if (!buffer)
    {
        fclose(file);
        return false;
    }
    const size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (bytes_read != (size_t)size)
    {
        free(buffer);
        return false;
    }
    if (!er_assets_load_pack(buffer, (size_t)size))
    {
        free(buffer); /* malformed / partially written — keep whatever was registered before */
        return false;
    }
    return true;
}
