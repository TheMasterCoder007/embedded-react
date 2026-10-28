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
 * baker (packages/embedded-react/assets/emit-pack.mjs). Parses a pack from a memory buffer and registers
 * its images and fonts with the engine (er_image_load / er_font_register). Platform-neutral and
 * filesystem-free — the buffer can be a file read into RAM (simulator) or a pointer into flash (a
 * config container on a device). It is also the asset section of the ERCF config container.
 *
 * The buffer must stay live for as long as the assets are used: image pixels and font bitmaps are
 * referenced by pointer into it. er_assets_load_pack's per-font GlyphInfo/ExtraGlyph arrays + BitmapFont
 * structs are heap-allocated (er_assets_heap.c) and kept for the process (a reload leaks the previous set rather than
 * risk a dangling reference — the engine has no "unregister"; fine for the few-KB-per-reload dev/config-swap case).
 *
 * For a VOLATILE source — a hot-reload staging buffer that gets reused for the next upload — use
 * er_assets_load_pack_ex(buf, len, copy=true): it takes its own (reused) copy of the pack and points the
 * engine at that, so the caller's buffer is free the moment the load returns. The plain
 * er_assets_load_pack() is the zero-copy path (copy=false): assets are referenced in @p buf in place, so
 * a config mmap'd from flash costs no RAM for its pixels — keep it for production boot + desktop.
 */

#include "er_scene.h" /* BitmapFont, GlyphInfo, ExtraGlyph */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What a pack holds, as er_assets_check_pack counts it. */
typedef struct
{
    uint32_t images;         /**< Images. */
    uint32_t fonts;          /**< Fonts (one per family and size). */
    uint32_t glyphs;         /**< Glyph records, over every font. */
    uint32_t extra_glyphs;   /**< Extra-glyph records (codepoints past a font's range), over every font. */
    uint32_t misaligned;     /**< Images whose pixels don't sit on their word size (only a v1 pack can have them). */
    uint32_t empty_images;   /**< Images with no width or no height, which the engine won't register. */
    uint32_t font_families;  /**< Distinct family names, as the font registry compares them. */
    uint32_t largest_family; /**< The most sizes one family has. */
} ErAssetsInfo;

/** @brief Caller storage for the font tables a pack registers, for a firmware with no heap. */
typedef struct
{
    BitmapFont* fonts;    /**< One per font. */
    uint32_t font_count;  /**< Length of fonts. */
    GlyphInfo* glyphs;    /**< Glyph records. */
    uint32_t glyph_count; /**< Length of glyphs. */
    ExtraGlyph* extras;   /**< Extra-glyph records. */
    uint32_t extra_count; /**< Length of extras. */
    uint32_t fonts_used;  /**< Set by er_assets_load_pack_into: entries it took. */
    uint32_t glyphs_used;
    uint32_t extras_used;
} ErAssetsStorage;

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

/**
 * @brief Checks a pack without registering anything, and counts what it holds.
 *
 * @param[in]  buf   Pack bytes.
 * @param[in]  len   Byte length.
 * @param[out] info  Receives the counts (may be NULL).
 *
 * Every font must be drawable: a known format, a dense glyph array that covers [first, last], every glyph's
 * bitmap inside the font's, and extra glyphs in ascending codepoint order.
 *
 * @return true when the whole pack reads cleanly, so loading it can't fail part way for its own sake.
 */
bool er_assets_check_pack(const void* buf, size_t len, ErAssetsInfo* info);

/**
 * @brief Reports whether a pack holds an image by this name.
 *
 * @param[in] buf   Pack bytes.
 * @param[in] len   Byte length.
 * @param[in] name  Null-terminated image name.
 *
 * @return true when the pack is well formed and has the image.
 */
bool er_assets_pack_has_image(const void* buf, size_t len, const char* name);

/**
 * @brief Like er_assets_load_pack (in place), with font tables taken from @p storage instead of the heap.
 *
 * @param[in]     buf      Pack bytes (caller-owned; must outlive use).
 * @param[in]     len      Byte length.
 * @param[in,out] storage  Font table storage; its *_used fields report what was taken.
 *
 * @return true if parsed and registered; false on a malformed pack or a font table that doesn't fit.
 */
bool er_assets_load_pack_into(const void* buf, size_t len, ErAssetsStorage* storage);

#endif
