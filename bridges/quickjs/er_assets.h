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

#ifndef ER_ASSETS_H
#define ER_ASSETS_H

/*
 * er_assets — portable loader for the ERPK binary asset pack (images + fonts), produced by the JS
 * baker (bridges/quickjs/js/assets/emit-pack.mjs). Parses a pack from a memory buffer and registers
 * its images and fonts with the engine (er_image_load / er_font_register). Platform-neutral and
 * filesystem-free — the buffer can be a file read into RAM (simulator) or a pointer into flash (a
 * config container on a device). It is also the asset section of the ERCF config container.
 *
 * The buffer must stay live for as long as the assets are used: image pixels and font bitmaps are
 * referenced by pointer into it. Per-font GlyphInfo/ExtraGlyph arrays + BitmapFont structs are
 * heap-allocated and kept for the process (a reload leaks the previous set rather than risk a dangling
 * reference — the engine has no "unregister"; fine for the few-KB-per-reload dev/config-swap case).
 *
 * For a VOLATILE source — a hot-reload staging buffer that gets reused for the next upload — use
 * er_assets_load_pack_ex(buf, len, copy=true): it takes its own (reused) copy of the pack and points the
 * engine at that, so the caller's buffer is free the moment the load returns. The plain
 * er_assets_load_pack() is the zero-copy path (copy=false): assets are referenced in @p buf in place, so
 * a config mmap'd from flash costs no RAM for its pixels — keep it for production boot + desktop.
 */

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Parses an ERPK asset pack from memory and registers its images and fonts with the engine
 *        (zero-copy: references @p buf in place — see header note). Equivalent to er_assets_load_pack_ex
 *        with copy=false; preserved as the original, unchanged entry point.
 *
 * @param[in] buf  Pack bytes (caller-owned; must outlive use).
 * @param[in] len  Byte length.
 *
 * @return true if parsed and registered; false on a malformed/truncated pack (a wrong magic or an
 *         over-read leaves whatever was registered before untouched).
 */
bool er_assets_load_pack(const void* buf, size_t len);

/**
 * @brief Like er_assets_load_pack, but optionally takes its own copy of the pack first.
 *
 * @param[in] buf   Pack bytes.
 * @param[in] len   Byte length.
 * @param[in] copy  true → copy the pack into a persistent (reused) block and register the engine against
 *                  the copy, so @p buf may be reused/freed immediately (hot reload from a staging buffer).
 *                  false → reference @p buf in place (identical to er_assets_load_pack).
 *
 * @return true if parsed and registered; false on a malformed pack or (copy=true) an allocation failure.
 */
bool er_assets_load_pack_ex(const void* buf, size_t len, bool copy);

#endif
