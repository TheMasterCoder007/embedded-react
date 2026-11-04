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

#include "er_scene.h"
#include "font_blob.h"
#include "font_registry.h"
#include "image_registry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*----------------------------------------------------------------------------------------------------------------------
 - Test helpers
 ---------------------------------------------------------------------------------------------------------------------*/

static int s_fail = 0;

#define CHECK(condition, message)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL: %s\n", (message));                                                                           \
            s_fail = 1;                                                                                                \
        }                                                                                                              \
    } while (0)

/**
 * @brief Writes a little-endian uint16_t into a byte buffer.
 *
 * @param[out] destination  Destination (2 bytes).
 * @param[in]  value        Value to write.
 */
static void put_u16_le(uint8_t* destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/**
 * @brief Writes a little-endian uint32_t into a byte buffer.
 *
 * @param[out] destination  Destination (4 bytes).
 * @param[in]  value        Value to write.
 */
static void put_u32_le(uint8_t* destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)((value >> 8) & 0xFFU);
    destination[2] = (uint8_t)((value >> 16) & 0xFFU);
    destination[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief Builds a minimal valid FONT wire-format blob into destination.
 *
 * Produces a single-glyph (or multi-glyph) version-2 font with a solid bitmap so the blob
 * passes font_blob_register()'s validation. The total size is returned via out_size.
 *
 * @param[out] destination  Destination buffer (must be large enough).
 * @param[in]  first        First character code.
 * @param[in]  last         Last character code (>= first).
 * @param[in]  pixel_size   Nominal pixel size.
 * @param[out] out_size     Receives the total blob size in bytes.
 */
static void build_blob(uint8_t* destination, uint8_t first, uint8_t last, uint8_t pixel_size, uint32_t* out_size)
{
    const uint16_t glyph_count = (uint16_t)(last - first + 1U);
    const uint32_t bitmap_size = 4U;

    destination[0] = FONT_BLOB_MAGIC_0;
    destination[1] = FONT_BLOB_MAGIC_1;
    destination[2] = FONT_BLOB_MAGIC_2;
    destination[3] = FONT_BLOB_MAGIC_3;
    destination[FONT_BLOB_VERSION_AT] = FONT_BLOB_VERSION;
    destination[FONT_BLOB_FIRST_AT] = first;
    destination[FONT_BLOB_LAST_AT] = last;
    destination[FONT_BLOB_PIXEL_SIZE_AT] = pixel_size;
    destination[FONT_BLOB_LINE_HEIGHT_AT] = 18U;
    destination[FONT_BLOB_BASELINE_AT] = 14U;
    destination[FONT_BLOB_FORMAT_AT] = 0U;
    put_u16_le(&destination[FONT_BLOB_GLYPH_COUNT_AT], glyph_count);
    put_u32_le(&destination[FONT_BLOB_BITMAP_SIZE_AT], bitmap_size);

    uint8_t* glyph_record = &destination[FONT_BLOB_HEADER_SIZE];
    for (uint16_t glyph_index = 0; glyph_index < glyph_count; glyph_index++)
    {
        put_u16_le(&glyph_record[0], 0U); /* bitmap_offset */
        glyph_record[FONT_BLOB_GLYPH_WIDTH_AT] = 2U;
        glyph_record[FONT_BLOB_GLYPH_HEIGHT_AT] = 2U;
        glyph_record[FONT_BLOB_GLYPH_X_OFFSET_AT] = 0U;
        glyph_record[FONT_BLOB_GLYPH_Y_OFFSET_AT] = 0U;
        glyph_record[FONT_BLOB_GLYPH_ADVANCE_AT] = 3U;
        glyph_record[7] = 0U; /* unused */
        glyph_record += FONT_BLOB_GLYPH_BYTES;
    }
    for (uint32_t byte_index = 0; byte_index < bitmap_size; byte_index++)
        glyph_record[byte_index] = 0xFFU;

    *out_size = FONT_BLOB_HEADER_SIZE + (uint32_t)glyph_count * FONT_BLOB_GLYPH_BYTES + bitmap_size;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Image registry replace
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies er_image_load replaces an existing name in place without consuming a new slot.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_image_replace(void)
{
    image_registry_init();

    static uint32_t pixels_a[4] = {0xFF112233U, 0xFF112233U, 0xFF112233U, 0xFF112233U};
    static uint32_t pixels_b[1] = {0xFFAABBCCU};

    er_image_load("logo", pixels_a, 2, 2);
    const ImageEntry* entry = image_registry_get("logo");
    CHECK(entry != NULL && entry->width == 2 && entry->height == 2 && entry->pixels == pixels_a,
          "image: initial registration");

    /* Re-register the same name with a different buffer and dimensions. */
    er_image_load("logo", pixels_b, 1, 1);
    entry = image_registry_get("logo");
    CHECK(entry != NULL && entry->width == 1 && entry->height == 1 && entry->pixels == pixels_b,
          "image: replace in place");

    /* Fill the registry to capacity, then re-register an existing name: must still succeed
     * (replace does not consume a free slot). */
    char name[16];
    for (int slot_index = 1; slot_index < (int)ERUI_IMAGE_REGISTRY_MAX; slot_index++) /* slot 0 holds "logo" */
    {
        snprintf(name, sizeof(name), "img%d", slot_index);
        const bool ok = image_registry_store(name, pixels_a, 1, 1, ER_IMAGE_ARGB8888);
        CHECK(ok, "image: fill to capacity");
    }
    CHECK(image_registry_store("logo", pixels_a, 2, 2, ER_IMAGE_ARGB8888), "image: replace when registry is full");
    CHECK(!image_registry_store("overflow", pixels_a, 1, 1, ER_IMAGE_ARGB8888), "image: new name rejected when full");

    /* The pool really is ERUI_IMAGE_REGISTRY_MAX slots deep, not the header's #ifndef fallback. The
     * define is PUBLIC precisely so a build that tunes the knob moves this loop with it; were it
     * PRIVATE, this test would fill to 128 against a 32-slot library and fail at slot 32 instead. */
    CHECK(image_registry_in_use() == ERUI_IMAGE_REGISTRY_MAX, "image: in-use count matches the configured capacity");

    /* Everything registered before the pool filled is still addressable — overflow refuses the new
     * image, it does not evict an old one. Check both ends of the pool. */
    snprintf(name, sizeof(name), "img%d", (int)ERUI_IMAGE_REGISTRY_MAX - 1); /* the last slot claimed */
    CHECK(image_registry_get("logo") != NULL, "image: the first entry survived the overflow");
    CHECK(ERUI_IMAGE_REGISTRY_MAX < 2U || image_registry_get(name) != NULL,
          "image: the last entry survived the overflow");
    CHECK(image_registry_get("overflow") == NULL, "image: the refused image is genuinely absent");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Image registry format + opacity scan
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies the registration-time opacity scan and the RGB565 format tagging.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_image_format_and_opacity(void)
{
    image_registry_init();

    static const uint32_t opaque_pixels[4] = {0xFF112233U, 0xFFFFFFFFU, 0xFF000000U, 0xFF445566U};
    static const uint32_t translucent_pixels[4] = {0xFF112233U, 0xFF445566U, 0x80102030U, 0xFF778899U};
    static const uint16_t rgb565_pixels[4] = {0xF800U, 0x07E0U, 0x001FU, 0xFFFFU};

    er_image_load("opaque", opaque_pixels, 2, 2);
    const ImageEntry* entry = image_registry_get("opaque");
    CHECK(entry && entry->format == ER_IMAGE_ARGB8888 && entry->opaque, "format: fully opaque ARGB scans as opaque");

    er_image_load("translucent", translucent_pixels, 2, 2);
    entry = image_registry_get("translucent");
    CHECK(entry && entry->format == ER_IMAGE_ARGB8888 && !entry->opaque,
          "format: one translucent pixel scans as non-opaque");

    er_image_load_rgb565("bg565", rgb565_pixels, 2, 2);
    entry = image_registry_get("bg565");
    CHECK(entry && entry->format == ER_IMAGE_RGB565 && entry->opaque, "format: RGB565 registers as opaque 565");

    /* Re-registering an existing name with new pixels must re-scan and can change format. */
    er_image_load("bg565", translucent_pixels, 2, 2);
    entry = image_registry_get("bg565");
    CHECK(entry && entry->format == ER_IMAGE_ARGB8888 && !entry->opaque, "format: replace re-scans opacity and format");

    /* An out-of-range format must be rejected before it decides the buffer's element size. */
    CHECK(!image_registry_store("badfmt", opaque_pixels, 2, 2, (ERImageFormat)7), "format: unknown format rejected");
    CHECK(image_registry_get("badfmt") == NULL, "format: rejected format stored nothing");

    /* RGB565 buffers are read as uint16_t: a misaligned pointer must be rejected up front. */
    const uint8_t* misaligned = (const uint8_t*)rgb565_pixels + 1;
    CHECK(!image_registry_store("badalign", misaligned, 1, 1, ER_IMAGE_RGB565),
          "format: misaligned RGB565 buffer rejected");
    CHECK(image_registry_get("badalign") == NULL, "format: rejected alignment stored nothing");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Font registry replace
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies font_registry_add replaces a same-pixel_size entry instead of appending.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_font_registry_replace(void)
{
    font_registry_init();

    BitmapFont font_16_a = {0};
    font_16_a.pixel_size = 16U;
    font_16_a.line_height = 20U;
    BitmapFont font_16_b = {0};
    font_16_b.pixel_size = 16U;
    font_16_b.line_height = 24U;
    BitmapFont font_24 = {0};
    font_24.pixel_size = 24U;
    font_24.line_height = 30U;

    CHECK(font_registry_add("Fam", &font_16_a), "registry: add 16a");
    CHECK(font_registry_get_exact("Fam", 16U) == &font_16_a, "registry: exact returns 16a");

    /* Same name + same pixel_size replaces in place. */
    CHECK(font_registry_add("Fam", &font_16_b), "registry: replace 16");
    CHECK(font_registry_get_exact("Fam", 16U) == &font_16_b, "registry: exact returns 16b after replace");

    /* A different pixel_size is a distinct entry, not a replacement. */
    CHECK(font_registry_add("Fam", &font_24), "registry: add 24");
    CHECK(font_registry_get_exact("Fam", 24U) == &font_24, "registry: exact returns 24");
    CHECK(font_registry_get_exact("Fam", 16U) == &font_16_b, "registry: 16 still present after adding 24");

    /* Replacing the same (name, size) many times must never exhaust the size table. */
    for (int repeat = 0; repeat < 100; repeat++)
        CHECK(font_registry_add("Fam", &font_16_a), "registry: repeated replace must not overflow");
    CHECK(font_registry_get_exact("Fam", 16U) == &font_16_a, "registry: final replace wins");

    /* Unknown lookups return NULL (no closest-match, no fallback). */
    CHECK(font_registry_get_exact("Fam", 99U) == NULL, "registry: exact miss on size");
    CHECK(font_registry_get_exact("Nope", 16U) == NULL, "registry: exact miss on name");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Font blob pool reuse
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies that re-registering the same (family, size) reuses pool bytes when it fits.
 *
 * Meaningful only when the font is built with ERUI_FONT_POOL_BYTES > 0; when the pool is
 * disabled, font_blob_register returns an error and the byte-level assertions are skipped
 * (the test still passes). The registry-replace behavior is covered by test_font_registry_replace.
 *
 * @return 0 on success, 1 on failure.
 */
static int test_font_blob_reuse(void)
{
    font_registry_init();
    font_blob_init(0U);

    /* Large enough for the biggest blob built below: 17 header + 49*8 glyph + 4 bitmap = 413. */
    uint8_t blob[512];
    uint32_t size = 0;
    build_blob(blob, 32U, 33U, 16U, &size); /* 2 glyphs at size 16 */

    const FontBlobStatus status = font_blob_register("ReuseFont", blob, size);
    if (status != FONT_BLOB_OK)
    {
        /* Pool disabled at build time — nothing to exercise here. */
        return s_fail;
    }

    const uint32_t used1 = font_blob_used_bytes();
    CHECK(used1 > 0U, "blob: first registration consumed pool bytes");

    /* Identical re-register of the same (name, size): must repack in place, no growth. */
    CHECK(font_blob_register("ReuseFont", blob, size) == FONT_BLOB_OK, "blob: re-register succeeds");
    const uint32_t used2 = font_blob_used_bytes();
    CHECK(used2 == used1, "blob: identical re-register must not grow the pool");
    CHECK(font_registry_get_exact("ReuseFont", 16U) != NULL, "blob: font retrievable after reuse");

    /* A smaller blob for the same (name, size) also fits within the prior footprint. */
    uint32_t small_size = 0;
    build_blob(blob, 32U, 32U, 16U, &small_size); /* 1 glyph */
    CHECK(font_blob_register("ReuseFont", blob, small_size) == FONT_BLOB_OK, "blob: smaller re-register succeeds");
    CHECK(font_blob_used_bytes() == used1, "blob: smaller re-register stays within reserved footprint");

    /* A larger blob for the same (name, size) cannot fit: fresh bytes are consumed. */
    uint32_t big_size = 0;
    build_blob(blob, 32U, 80U, 16U, &big_size); /* 49 glyphs */
    CHECK(font_blob_register("ReuseFont", blob, big_size) == FONT_BLOB_OK, "blob: larger re-register succeeds");
    CHECK(font_blob_used_bytes() > used1, "blob: larger re-register allocates fresh bytes");
    CHECK(font_registry_get_exact("ReuseFont", 16U) != NULL, "blob: font retrievable after fresh alloc");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Tests: Public er_font_register
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Verifies the public er_font_register() registers a baked font by pointer (zero-copy) and is
 *        resolvable by closest-size lookup, with NULL arguments treated as safe no-ops.
 *
 * This is the build-time font path the converter's generated C uses (no font pool, no copy).
 *
 * @return 0 on success, 1 on failure.
 */
static int test_font_register_public(void)
{
    font_registry_init();

    BitmapFont brand16 = {0};
    brand16.pixel_size = 16U;
    brand16.line_height = 20U;
    BitmapFont brand24 = {0};
    brand24.pixel_size = 24U;
    brand24.line_height = 30U;

    er_font_register("Brand", &brand16);
    er_font_register("Brand", &brand24);

    /* The exact baked struct is returned by pointer (never copied). */
    CHECK(font_registry_get_exact("Brand", 16U) == &brand16, "er_font_register: exact 16 by pointer");
    CHECK(font_registry_get_exact("Brand", 24U) == &brand24, "er_font_register: exact 24 by pointer");

    /* A fontSize between baked sizes resolves to the nearest registered one (no scaling). */
    CHECK(font_registry_get("Brand", 17U) == &brand16, "er_font_register: 17 picks nearest (16)");
    CHECK(font_registry_get("Brand", 22U) == &brand24, "er_font_register: 22 picks nearest (24)");

    /* NULL family or font are safe no-ops and must not register anything or crash. */
    er_font_register(NULL, &brand16);
    er_font_register("Bogus", NULL);
    CHECK(font_registry_get_exact("Bogus", 16U) == NULL, "er_font_register: NULL font did not register");

    /* An unknown family falls back to the built-in font (never NULL). */
    CHECK(font_registry_get("Unknown", 16U) != NULL, "er_font_register: unknown family falls back to built-in");

    return s_fail;
}

/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/

/**
 * @brief Test entry point for resource registry teardown/replace behavior.
 *
 * @return 0 on success, 1 on any failure.
 */
int main(void)
{
    (void)test_image_replace();
    (void)test_image_format_and_opacity();
    (void)test_font_registry_replace();
    (void)test_font_blob_reuse();
    (void)test_font_register_public();

    if (s_fail)
        return 1;

    printf("OK\n");
    return 0;
}
