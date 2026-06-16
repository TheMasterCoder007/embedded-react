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

#ifndef ER_SIM_ASSET_PACK_H
#define ER_SIM_ASSET_PACK_H

#include <stdbool.h>

/*
 * Runtime asset-pack loader for the simulator (see tools/simulator/README.md). Reads an ERPK binary
 * pack (produced by the JS bakers via bridges/quickjs/js/assets/emit-pack.mjs) and registers its
 * images and fonts with the engine (er_image_load / er_font_register). This is how the simulator
 * hot-reloads assets without a rebuild; on a real device, assets stay baked into the binary.
 */

/**
 * @brief Loads an ERPK asset pack and (re)registers its images and fonts with the engine.
 *
 * Safe to call repeatedly (the engine replaces same-named images / same (family,size) fonts in
 * place), so this is the asset hot-reload entry point. Buffers are kept alive for the process; a
 * reload allocates fresh ones and intentionally leaks the previous set (a few KB) rather than risk
 * dangling a still-referenced asset — fine for a dev tool. A missing or malformed/partially-written
 * pack is a no-op that returns false (the previously loaded assets stay in place).
 *
 * @param[in] path  Path to the .pack file.
 *
 * @return true if the pack was loaded and registered; false on a missing/invalid pack.
 */
bool er_asset_pack_load(const char* path);

#endif
